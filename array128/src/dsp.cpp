#include "dsp.h"
#include "protocol.h"
#include "localizationfft.h"
#include "analysis/directionprojection.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace MicArray::Array128 {
bool onlyDirectionChange(const ProcessingSettings &before, const ProcessingSettings &after)
{
    auto normalized = before;
    normalized.directions = after.directions;
    normalized.generation = after.generation;
    return normalized == after;
}

namespace {
constexpr double Pi = 3.14159265358979323846;
std::array<double, 3> unit(const ScanDirection &direction)
{
    const double az = direction.azimuthDegrees * Pi / 180, el = direction.elevationDegrees * Pi / 180;
    return {std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)};
}
double dot(const MicrophonePosition &p, const std::array<double, 3> &d)
{
    return p.xMeters * d[0] + p.yMeters * d[1] + p.zMeters * d[2];
}
}

LocalizationResult localize(const QVector<QVector<float>> &channels, const ProcessingSettings &settings)
{
    QElapsedTimer timer;
    timer.start();
    LocalizationResult result;
    result.generation = settings.generation;
    if (!settings.camera.isValid() || !std::isfinite(settings.soundSpeed) || settings.soundSpeed < 300 || settings.soundSpeed > 380
        || !std::isfinite(settings.confidenceThreshold) || settings.confidenceThreshold < 0 || settings.confidenceThreshold > 1) {
        result.reason = QStringLiteral("视角、声速或可信阈值无效");
        return result;
    }
    if (!settings.geometry.valid() || channels.size() != Channels || channels[0].size() < 1024) {
        result.reason = QStringLiteral("缺少真实阵元坐标或连续音频窗口");
        return result;
    }
    constexpr int N = 2048;
    for (const auto &p : settings.geometry.microphones) {
        if (!std::isfinite(p.xMeters) || !std::isfinite(p.yMeters) || !std::isfinite(p.zMeters)
            || std::abs(p.xMeters) > 2 || std::abs(p.yMeters) > 2 || std::abs(p.zMeters) > 2) {
            result.reason = QStringLiteral("阵元坐标非有限值或超出 ±2 m");
            return result;
        }
    }
    using Complex = std::complex<double>;
    QVector<QVector<Complex>> spectra(Channels);
    QVector<double> energies(Channels);
    for (int ch = 0; ch < Channels; ++ch) {
        if (channels[ch].size() < N) { result.reason = QStringLiteral("窗口不足 2048 样本"); return result; }
        spectra[ch].resize(N);
        if (!std::all_of(channels[ch].cbegin(), channels[ch].cbegin() + N, [](float v) { return std::isfinite(v); })) continue;
        double mean = std::accumulate(channels[ch].cbegin(), channels[ch].cbegin() + N, 0.0) / N;
        for (int n = 0; n < N; ++n) {
            const double x = channels[ch][n] - mean;
            energies[ch] += x * x / N;
            spectra[ch][n] = x * (0.5 - 0.5 * std::cos(2 * Pi * n / (N - 1)));
        }
        LocalizationFft::transform(&spectra[ch], false);
    }
    auto sorted = energies;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[Channels / 2];
    if (median < 1e-12) { result.reason = QStringLiteral("静音或交流能量不足"); return result; }
    QVector<bool> good(Channels);
    for (int ch = 0; ch < Channels; ++ch) {
        good[ch] = energies[ch] > median * 0.01 && energies[ch] < median * 100;
        result.validChannels += good[ch];
    }
    if (result.validChannels < 64) { result.reason = QStringLiteral("有效通道少于 64"); return result; }
    struct Pair { MicrophonePosition displacement; QVector<double> correlation; };
    QVector<Pair> pairs;
    // Four coprime separations cover every input channel while bounding CPU cost.
    // Geometry, not channel index, determines the steering delays.
    const int separations[] {1, 7, 31, 63};
    double totalSpectralEnergy = 0, strongestBin = 0;
    for (int k = 11; k <= 341; ++k) { // ~250..8000 Hz, hardware valid band <=16 kHz
        double power = 0;
        for (int ch = 0; ch < Channels; ++ch) if (good[ch]) power += std::norm(spectra[ch][k]);
        totalSpectralEnergy += power;
        strongestBin = std::max(strongestBin, power);
    }
    if (strongestBin > totalSpectralEnergy * 0.45) {
        result.reason = QStringLiteral("窄带歧义：无法可靠区分空间旁瓣");
        return result;
    }
    for (int separation : separations) for (int a = 0; a < Channels; a += 2) {
        const int b = (a + separation) % Channels;
        if (!good[a] || !good[b]) continue;
        const auto &pa = settings.geometry.microphones[a], &pb = settings.geometry.microphones[b];
        Pair pair {{pa.xMeters - pb.xMeters, pa.yMeters - pb.yMeters, pa.zMeters - pb.zMeters}, {}};
        const double maxLag = std::sqrt(dot(pair.displacement, {pair.displacement.xMeters, pair.displacement.yMeters, pair.displacement.zMeters})) * SampleRate / settings.soundSpeed;
        if (maxLag >= N / 2 - 1) continue; // Circular GCC cannot resolve a baseline exceeding half the window.
        QVector<Complex> cross(N);
        double maxCross = 0;
        for (int k = 11; k <= 341; ++k) maxCross = std::max(maxCross, std::abs(spectra[a][k] * std::conj(spectra[b][k])));
        for (int k = 11; k <= 341; ++k) {
            Complex value = spectra[a][k] * std::conj(spectra[b][k]);
            double magnitude = std::abs(value);
            if (magnitude > maxCross * 1e-4 && magnitude > 1e-18) {
                cross[k] = value / magnitude;
                cross[N - k] = std::conj(cross[k]);
            }
        }
        LocalizationFft::transform(&cross, true);
        pair.correlation.resize(N);
        for (int n = 0; n < N; ++n) pair.correlation[n] = cross[n].real();
        pairs.append(std::move(pair));
    }
    result.validPairs = int(pairs.size());
    if (pairs.size() < 64) { result.reason = QStringLiteral("有效基线不足"); return result; }
    auto correlationAt = [](const Pair &pair, double lag) {
        const int lower = int(std::floor(lag));
        const double f = lag - lower;
        const int index = (lower + N) & (N - 1);
        return pair.correlation[index] * (1 - f) + pair.correlation[(index + 1) & (N - 1)] * f;
    };
    const double samplesPerMeter = SampleRate / settings.soundSpeed;
    auto score = [&](const ScanDirection &direction) {
        const auto d = unit(direction);
        double sum = 0;
        for (const auto &pair : pairs) {
            // d points TOWARD the source, not along wave propagation. A nearer
            // microphone has tau=-dot(position,d)/c; GCC peaks at tau_a-tau_b.
            sum += correlationAt(pair, -dot(pair.displacement, d) * samplesPerMeter);
        }
        return sum / pairs.size();
    };
    result.columns = std::clamp(settings.gridColumns, 24, 160);
    result.rows = std::clamp(settings.gridRows, 15, 100);
    result.heat.resize(result.columns * result.rows);
    QVector<ScanDirection> gridDirections(result.heat.size());
    QVector<std::array<double, 3>> gridUnits(result.heat.size());
    QVector<double> response(result.heat.size(), 0);
    for (int y = 0; y < result.rows; ++y) for (int x = 0; x < result.columns; ++x) {
        const int index = y * result.columns + x;
        gridDirections[index] = unprojectVideoPoint(QPointF(double(x) / (result.columns - 1), double(y) / (result.rows - 1)), settings.camera);
        gridUnits[index] = unit(gridDirections[index]);
    }
    const int displaySamples = int(gridDirections.size());
    auto projection = [&](const ScanDirection &direction) {
        SoundSourceDirection source;
        source.azimuthDegrees = direction.azimuthDegrees; source.elevationDegrees = direction.elevationDegrees;
        return projectDirection(source, settings.camera);
    };
    // A rectilinear image grid is not an angular search grid. At 170 degrees,
    // even the two central pixels can be tens of degrees apart. Add an angular
    // search lattice independent of display resolution. Search the full front
    // hemisphere so an out-of-view source is not misreported as a trusted edge.
    for (int el = -87; el <= 87; el += 3) for (int az = -87; az <= 87; az += 3) {
        const ScanDirection direction {double(az), double(el)};
        gridDirections.append(direction); gridUnits.append(unit(direction)); response.append(0);
    }
    // Pair-major traversal keeps one 16 KiB GCC array hot in cache instead of
    // jumping across 256 arrays for every pixel. The same physical scores feed
    // both the heatmap and direction search; this is not a synthetic heat blob.
    auto *sums = response.data(); const auto *units = gridUnits.constData(); const int scanCount = int(response.size());
    for (const auto &pair : pairs) {
        const MicrophonePosition delay {-pair.displacement.xMeters * samplesPerMeter,
            -pair.displacement.yMeters * samplesPerMeter, -pair.displacement.zMeters * samplesPerMeter};
        for (int i = 0; i < scanCount; ++i) sums[i] += correlationAt(pair, dot(delay, units[i]));
    }
    double peak = -1e30;
    ScanDirection best;
    auto preserveCellMaximum = [&](const ScanDirection &direction, double value) {
        const auto point = projection(direction);
        if (!point.inView) return;
        const int x = std::clamp(int(std::lround(point.normalizedPoint.x() * (result.columns - 1))), 0, result.columns - 1);
        const int y = std::clamp(int(std::lround(point.normalizedPoint.y() * (result.rows - 1))), 0, result.rows - 1);
        float &pixel = result.heat[y * result.columns + x];
        pixel = std::max(pixel, float(value));
    };
    for (int i = 0; i < response.size(); ++i) {
        const double value = response[i] / pairs.size();
        if (i < displaySamples) result.heat[i] = float(value);
        else preserveCellMaximum(gridDirections[i], value);
        if (value > peak) { peak = value; best = gridDirections[i]; }
    }
    // Continuous local search keeps the numeric direction independent of pixel quantization.
    for (double step : {2.0, 1.0, 0.5, 0.25}) {
        const ScanDirection center = best;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            ScanDirection trial {std::clamp(center.azimuthDegrees + dx * step, -89.9, 89.9),
                std::clamp(center.elevationDegrees + dy * step, -89.9, 89.9)};
            double value = score(trial);
            // Max-pool measured subpixel responses into the display cell. This
            // preserves a real peak on a coarse raster; no marker-shaped boost.
            preserveCellMaximum(trial, value);
            if (value > peak) { peak = value; best = trial; }
        }
    }
    auto values = result.heat;
    std::sort(values.begin(), values.end());
    const double background = values[values.size() / 2];
    const double contrast = std::clamp((peak - background) / std::max(std::abs(peak), 0.005), 0.0, 1.0);
    const double coherence = std::clamp(peak / 0.12, 0.0, 1.0);
    result.confidence = contrast * std::sqrt(coherence) * result.validChannels / Channels;
    const bool peakInView = projection(best).inView;
    result.trusted = peakInView && peak > 0.002 && result.confidence >= settings.confidenceThreshold;
    result.direction = best;
    const double scale = std::max(peak - background, 1e-9);
    for (float &value : result.heat) value = float(std::clamp((value - background) / scale, 0.0, 1.0));
    result.reason = !peakInView ? QStringLiteral("主峰位于视频视场外，请调整视角；不标记为画面内可信声源")
        : result.trusted ? QStringLiteral("可信候选方向") : QStringLiteral("相对峰存在，但未达到可信阈值");
    result.elapsedMs = timer.nsecsElapsed() / 1e6;
    return result;
}

void Beamformer::configure(const Geometry &geometry, const std::array<ScanDirection, 3> &directions, double speed)
{
    const bool positionsFinite = std::all_of(geometry.microphones.cbegin(), geometry.microphones.cend(), [](const MicrophonePosition &p) {
        return std::isfinite(p.xMeters) && std::isfinite(p.yMeters) && std::isfinite(p.zMeters)
            && std::abs(p.xMeters) <= 2 && std::abs(p.yMeters) <= 2 && std::abs(p.zMeters) <= 2;
    });
    const bool directionsFinite = std::all_of(directions.cbegin(), directions.cend(), [](const ScanDirection &d) {
        return std::isfinite(d.azimuthDegrees) && std::isfinite(d.elevationDegrees);
    });
    if (!geometry.valid() || !positionsFinite || !directionsFinite || !std::isfinite(speed) || speed < 300 || speed > 380) { m_ring.clear(); return; }
    double radius = 0;
    for (const auto &p : geometry.microphones) radius = std::max(radius,
        std::sqrt(p.xMeters * p.xMeters + p.yMeters * p.yMeters + p.zMeters * p.zMeters));
    const double reference = std::ceil(radius * SampleRate / speed) + 3;
    const int size = int(2 * reference) + 8;
    if (geometry.microphones != m_geometry.microphones || speed != m_speed || size != m_ringSize) {
        m_geometry = geometry;
        m_speed = speed;
        m_ringSize = size;
        m_ring = QVector<QVector<float>>(Channels, QVector<float>(size, 0));
        m_position = 0;
    }
    for (int beam = 0; beam < 3; ++beam) {
        const auto d = unit(directions[beam]);
        m_taps[beam].resize(Channels);
        for (int ch = 0; ch < Channels; ++ch) {
            // Delay early-arriving microphones more so a source in +d sums coherently.
            const double delay = reference + dot(geometry.microphones[ch], d) * SampleRate / speed;
            auto &tap = m_taps[beam][ch];
            tap.integer = int(std::floor(delay)) - 1;
            const double x = delay - tap.integer;
            for (int i = 0; i < 4; ++i) {
                double weight = 1;
                for (int j = 0; j < 4; ++j) if (i != j) weight *= (x - j) / (i - j);
                tap.weights[i] = weight;
            }
        }
    }
}

void Beamformer::reset()
{
    for (auto &channel : m_ring) channel.fill(0);
    m_position = 0;
}

std::array<QVector<float>, 3> Beamformer::process(const QVector<QVector<float>> &channels)
{
    std::array<QVector<float>, 3> output;
    if (m_ring.size() != Channels || channels.size() != Channels) return output;
    const int count = int(channels[0].size());
    for (const auto &channel : channels) if (channel.size() != count) return output;
    for (auto &beam : output) beam.resize(count);
    for (int n = 0; n < count; ++n) {
        for (int ch = 0; ch < Channels; ++ch) m_ring[ch][m_position] = channels[ch][n];
        for (int beam = 0; beam < 3; ++beam) {
            double value = 0;
            for (int ch = 0; ch < Channels; ++ch) {
                const auto &tap = m_taps[beam][ch];
                for (int i = 0; i < 4; ++i) {
                    const int index = (m_position - tap.integer - i + m_ringSize) % m_ringSize;
                    value += tap.weights[i] * m_ring[ch][index];
                }
            }
            output[beam][n] = float(value / Channels);
        }
        m_position = (m_position + 1) % m_ringSize;
    }
    return output;
}

void Denoiser::configure(const ProcessingSettings &settings)
{
    if (m_configured && onlyDirectionChange(m_settings, settings)) {
        m_beamformer.configure(settings.geometry, settings.directions, settings.soundSpeed);
        m_settings = settings;
        return;
    }
    m_settings = settings;
    m_beamformer.configure(settings.geometry, settings.directions, settings.soundSpeed);
    m_wiener.setMode(settings.dual ? NoiseReductionMode::TwoPoint : NoiseReductionMode::SinglePoint);
    m_wiener.setAlgorithm(settings.adaptive ? NoiseReductionAlgorithm::AdaptiveWiener : NoiseReductionAlgorithm::FixedParameterWiener);
    m_wiener.setSuppressionCoefficients(settings.coefficientB1, settings.coefficientB1, settings.coefficientB2);
    m_bandpass.configure(SampleRate, settings.lowHz, settings.highHz);
    m_logmmse.configure(SampleRate, LogMmseSettings {});
    m_agc.configure(SampleRate, AutomaticGainControlSettings {});
    m_configured = true;
}

void Denoiser::reset()
{
    m_beamformer.reset();
    m_wiener.reset();
    m_bandpass.reset();
    m_logmmse.reset();
    m_agc.reset();
}

std::array<QVector<float>, 7> Denoiser::process(const QVector<QVector<float>> &channels)
{
    std::array<QVector<float>, 7> stages;
    auto beams = m_beamformer.process(channels);
    for (int i = 0; i < 3; ++i) stages[i] = std::move(beams[i]);
    if (stages[0].isEmpty()) return stages;
    auto output = m_wiener.process(stages[0], stages[1], stages[2]);
    stages[3] = output;
    for (int node : m_settings.postOrder) {
        if (node == 0 && m_settings.bandpass) output = m_bandpass.process(output);
        if (node == 1 && m_settings.logmmse) output = m_logmmse.process(output);
        if (node == 2 && m_settings.agc) output = m_agc.process(output);
        if (node >= 0 && node < 3) stages[4 + node] = output;
    }
    return stages;
}

} // namespace MicArray::Array128
