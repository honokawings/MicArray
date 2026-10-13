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

double angularDistance(const ScanDirection &a, const ScanDirection &b)
{
    const auto u = unit(a), v = unit(b);
    return std::acos(std::clamp(u[0] * v[0] + u[1] * v[1] + u[2] * v[2], -1.0, 1.0)) * 180 / Pi;
}
}

LocalizationResult localize(const QVector<QVector<float>> &channels, const ProcessingSettings &settings)
{
    QElapsedTimer timer;
    timer.start();
    LocalizationResult result;
    result.generation = settings.generation;
    result.disabledChannels = int(std::count(settings.disabledChannels.cbegin(), settings.disabledChannels.cend(), true));
    if (!settings.camera.isValid() || !std::isfinite(settings.soundSpeed) || settings.soundSpeed < 300 || settings.soundSpeed > 380
        || !std::isfinite(settings.confidenceThreshold) || settings.confidenceThreshold < 0 || settings.confidenceThreshold > 1
        || settings.maxSources < 1 || settings.maxSources > 16
        || !std::isfinite(settings.minimumSourceSeparationDegrees) || settings.minimumSourceSeparationDegrees < 2
        || settings.minimumSourceSeparationDegrees > 90) {
        result.reason = QStringLiteral("视角、声速、可信阈值或多目标参数无效");
        return result;
    }
    if (!settings.geometry.valid() || channels.size() != Channels) {
        result.reason = QStringLiteral("缺少真实阵元坐标或连续音频窗口");
        return result;
    }
    constexpr int N = 2048;
    const int enabledCount = Channels - result.disabledChannels;
    if (enabledCount < 16) { result.reason = QStringLiteral("屏蔽后启用通道少于 16，无法可靠进行二维多目标定位"); return result; }
    for (int ch = 0; ch < Channels; ++ch) {
        if (settings.disabledChannels[ch]) continue;
        const auto &p = settings.geometry.microphones[ch];
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
        if (settings.disabledChannels[ch]) continue; // Never inspect, transform or normalize a masked input.
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
    QVector<double> sorted;
    for (int ch = 0; ch < Channels; ++ch) if (!settings.disabledChannels[ch]) sorted.append(energies[ch]);
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    if (median < 1e-12) { result.reason = QStringLiteral("静音或交流能量不足"); return result; }
    QVector<bool> good(Channels);
    QVector<int> active;
    for (int ch = 0; ch < Channels; ++ch) {
        good[ch] = !settings.disabledChannels[ch] && energies[ch] > median * 0.01 && energies[ch] < median * 100;
        result.validChannels += good[ch];
        if (good[ch]) active.append(ch);
    }
    if (result.validChannels < 16) { result.reason = QStringLiteral("有效通道少于 16，检查屏蔽列表及阵元交流能量"); return result; }
    // A healthy channel count alone does not establish angular observability.
    // Require two significant covariance axes after masking (planar arrays are valid).
    std::array<double, 3> center {};
    for (int ch : active) {
        const auto &p = settings.geometry.microphones[ch];
        center[0] += p.xMeters; center[1] += p.yMeters; center[2] += p.zMeters;
    }
    for (double &value : center) value /= active.size();
    double covariance[3][3] {};
    for (int ch : active) {
        const auto &p = settings.geometry.microphones[ch];
        const double v[] {p.xMeters - center[0], p.yMeters - center[1], p.zMeters - center[2]};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) covariance[i][j] += v[i] * v[j] / active.size();
    }
    double trace = 0, squareTrace = 0;
    for (int i = 0; i < 3; ++i) {
        trace += covariance[i][i];
        for (int j = 0; j < 3; ++j) squareTrace += covariance[i][j] * covariance[i][j];
    }
    if (trace < 1e-6 || (trace * trace - squareTrace) * .5 < trace * trace * .005) {
        result.reason = QStringLiteral("屏蔽后阵元近似共线或孔径不足，二维方向不可观测"); return result;
    }
    struct Pair { MicrophonePosition displacement; QVector<double> correlation; };
    QVector<Pair> pairs;
    // Reindex only healthy, enabled inputs. In particular, disabling all odd
    // channels must not remove every pair. Separation 1 covers every survivor.
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
    std::array<bool, Channels * Channels> seenPairs {};
    QVector<bool> covered(Channels, false);
    for (int separation : separations) for (int index = 0; index < active.size(); index += 2) {
        const int a = active[index], b = active[(index + separation) % active.size()];
        if (a == b) continue;
        const int pairKey = std::min(a, b) * Channels + std::max(a, b);
        if (seenPairs[pairKey]) continue;
        seenPairs[pairKey] = true;
        const auto &pa = settings.geometry.microphones[a], &pb = settings.geometry.microphones[b];
        Pair pair {{pa.xMeters - pb.xMeters, pa.yMeters - pb.yMeters, pa.zMeters - pb.zMeters}, {}};
        const double maxLag = std::sqrt(dot(pair.displacement, {pair.displacement.xMeters, pair.displacement.yMeters, pair.displacement.zMeters})) * SampleRate / settings.soundSpeed;
        if (maxLag < .001 || maxLag >= N / 2 - 1) continue; // Circular GCC cannot resolve an excessive baseline.
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
        covered[a] = covered[b] = true;
    }
    result.validPairs = int(pairs.size());
    if (pairs.size() < 24 || std::any_of(active.cbegin(), active.cend(), [&](int ch) { return !covered[ch]; })) {
        result.reason = QStringLiteral("有效基线不足或无法覆盖全部有效阵元"); return result;
    }
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
    // Continuous refinement is ordinary SRP optimization, not neural inference
    // or a claim of resolution beyond the array's acoustic information.
    auto refine = [&](ScanDirection &direction, double &value) {
        for (double step : {2.0, 1.0, 0.5, 0.25}) {
            const ScanDirection center = direction;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                ScanDirection trial {std::clamp(center.azimuthDegrees + dx * step, -89.9, 89.9),
                    std::clamp(center.elevationDegrees + dy * step, -89.9, 89.9)};
                const double measured = score(trial);
                preserveCellMaximum(trial, measured);
                if (measured > value) { value = measured; direction = trial; }
            }
        }
    };
    refine(best, peak);
    auto values = result.heat;
    std::sort(values.begin(), values.end());
    const double background = values[values.size() / 2];
    auto confidenceFor = [&](double value) {
        const double contrast = std::clamp((value - background) / std::max(std::abs(value), 0.005), 0.0, 1.0);
        const double coherence = std::clamp(value / 0.12, 0.0, 1.0);
        return contrast * std::sqrt(coherence) * result.validChannels / enabledCount;
    };
    result.confidence = confidenceFor(peak);
    bool peakInView = projection(best).inView;
    result.trusted = peakInView && peak > 0.002 && result.confidence >= settings.confidenceThreshold;
    result.direction = best;
    result.sources.append({best, result.confidence, 1.0, result.trusted});
    QVector<double> sourceResponses {peak};
    // Each secondary candidate must be a separate local maximum of the same
    // measured SRP response. Never fill maxSources with the strongest pixels:
    // those are usually neighbouring samples of one source's main lobe.
    QVector<int> ranking(response.size());
    std::iota(ranking.begin(), ranking.end(), 0);
    std::sort(ranking.begin(), ranking.end(), [&](int a, int b) { return response[a] > response[b]; });
    QVector<ScanDirection> inspected {best};
    const double candidateFloor = std::max(0.008, background + (peak - background) * .25);
    int refined = 0;
    for (int index : ranking) {
        if (result.sources.size() >= settings.maxSources || refined >= settings.maxSources * 8) break;
        double value = response[index] / pairs.size();
        if (value < candidateFloor) break;
        ScanDirection direction = gridDirections[index];
        auto near = [&](const ScanDirection &other) {
            return angularDistance(direction, other) < settings.minimumSourceSeparationDegrees;
        };
        if (std::any_of(inspected.cbegin(), inspected.cend(), near)) continue;
        ++refined;
        // Mark the seed even if refinement returns to an existing strong lobe.
        // Otherwise adjacent shoulder pixels repeatedly consume the bounded
        // search budget before a weaker, spatially separate source is visited.
        inspected.append(direction);
        refine(direction, value);
        if (std::any_of(result.sources.cbegin(), result.sources.cend(), [&](const AcousticSource &source) { return near(source.direction); })) continue;
        inspected.append(direction);
        // A shoulder of an already detected lobe can survive distance NMS.
        // Verify a closed neighbourhood on the sphere, not an az/el rectangle
        // (azimuth steps do not correspond to angular distance near the pole).
        const auto normal = unit(direction);
        const double az = direction.azimuthDegrees * Pi / 180;
        const std::array<double, 3> horizontal {std::cos(az), 0, -std::sin(az)};
        const std::array<double, 3> vertical {
            -normal[1] * std::sin(az), std::cos(direction.elevationDegrees * Pi / 180), -normal[1] * std::cos(az)};
        const double radius = std::clamp(settings.minimumSourceSeparationDegrees * .5, 2.0, 6.0) * Pi / 180;
        double neighbourhoodMaximum = -1e30;
        for (int sample = 0; sample < 12; ++sample) {
            const double phi = sample * 2 * Pi / 12;
            std::array<double, 3> v;
            for (int axis = 0; axis < 3; ++axis)
                v[axis] = normal[axis] * std::cos(radius) + std::sin(radius)
                    * (horizontal[axis] * std::cos(phi) + vertical[axis] * std::sin(phi));
            const ScanDirection neighbour {std::atan2(v[0], v[2]) * 180 / Pi, std::asin(std::clamp(v[1], -1.0, 1.0)) * 180 / Pi};
            neighbourhoodMaximum = std::max(neighbourhoodMaximum, score(neighbour));
        }
        if (value - neighbourhoodMaximum < std::max(.001, value * .03)) continue;
        const double confidence = confidenceFor(value);
        // The floor rejects diffuse noise and weak broadband sidelobes even if
        // the user deliberately lowers the display trust threshold to zero.
        if (confidence < .25 || value < candidateFloor) continue;
        const bool trusted = projection(direction).inView && confidence >= settings.confidenceThreshold;
        result.sources.append({direction, confidence, std::clamp((value - background) / std::max(peak - background, 1e-9), 0.0, 1.0), trusted});
        sourceResponses.append(value);
    }
    // Refinement can change the rank of nearly equal peaks. The compatibility
    // fields must still describe the strongest *refined* candidate.
    QVector<int> sourceRanking(result.sources.size());
    std::iota(sourceRanking.begin(), sourceRanking.end(), 0);
    std::stable_sort(sourceRanking.begin(), sourceRanking.end(), [&](int a, int b) { return sourceResponses[a] > sourceResponses[b]; });
    const auto unsortedSources = result.sources;
    peak = sourceResponses[sourceRanking.front()];
    for (int i = 0; i < sourceRanking.size(); ++i) {
        result.sources[i] = unsortedSources[sourceRanking[i]];
        result.sources[i].relativePower = std::clamp((sourceResponses[sourceRanking[i]] - background) / std::max(peak - background, 1e-9), 0.0, 1.0);
    }
    result.direction = result.sources.front().direction;
    result.confidence = result.sources.front().confidence;
    result.trusted = result.sources.front().trusted;
    peakInView = projection(result.direction).inView;
    const double scale = std::max(peak - background, 1e-9);
    for (float &value : result.heat) value = float(std::clamp((value - background) / scale, 0.0, 1.0));
    result.reason = !peakInView ? QStringLiteral("主峰位于视频视场外，请调整视角；不标记为画面内可信声源")
        : result.trusted ? QStringLiteral("检测到 %1 个可信声源候选").arg(std::count_if(result.sources.cbegin(), result.sources.cend(), [](const AcousticSource &source) { return source.trusted; }))
        : QStringLiteral("相对峰存在，但未达到可信阈值");
    result.elapsedMs = timer.nsecsElapsed() / 1e6;
    return result;
}

void Beamformer::configure(const Geometry &geometry, const std::array<ScanDirection, 3> &directions, double speed,
                          const std::array<bool, 128> &disabledChannels)
{
    QVector<int> enabled;
    bool positionsFinite = geometry.valid();
    for (int ch = 0; ch < geometry.microphones.size(); ++ch) {
        if (ch >= Channels || disabledChannels[ch]) continue;
        enabled.append(ch);
        const auto &p = geometry.microphones[ch];
        positionsFinite = positionsFinite && std::isfinite(p.xMeters) && std::isfinite(p.yMeters) && std::isfinite(p.zMeters)
            && std::abs(p.xMeters) <= 2 && std::abs(p.yMeters) <= 2 && std::abs(p.zMeters) <= 2;
    }
    const bool directionsFinite = std::all_of(directions.cbegin(), directions.cend(), [](const ScanDirection &d) {
        return std::isfinite(d.azimuthDegrees) && std::isfinite(d.elevationDegrees);
    });
    if (!geometry.valid() || !positionsFinite || enabled.isEmpty() || !directionsFinite || !std::isfinite(speed) || speed < 300 || speed > 380) { m_ring.clear(); m_enabledChannels.clear(); return; }
    double radius = 0;
    for (int ch : enabled) {
        const auto &p = geometry.microphones[ch];
        radius = std::max(radius, std::sqrt(p.xMeters * p.xMeters + p.yMeters * p.yMeters + p.zMeters * p.zMeters));
    }
    const double reference = std::ceil(radius * SampleRate / speed) + 3;
    const int size = int(2 * reference) + 8;
    if (m_ring.size() != Channels || geometry.microphones != m_geometry.microphones || speed != m_speed
        || size != m_ringSize || disabledChannels != m_disabledChannels) {
        m_geometry = geometry;
        m_speed = speed;
        m_ringSize = size;
        m_ring = QVector<QVector<float>>(Channels, QVector<float>(size, 0));
        m_position = 0;
    }
    m_disabledChannels = disabledChannels;
    m_enabledChannels = enabled;
    for (int beam = 0; beam < 3; ++beam) {
        const auto d = unit(directions[beam]);
        m_taps[beam].resize(Channels);
        for (int ch : m_enabledChannels) {
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
    if (m_ring.size() != Channels || channels.size() != Channels || m_enabledChannels.isEmpty()) return output;
    const int count = int(channels[m_enabledChannels.front()].size());
    for (int ch : m_enabledChannels) if (channels[ch].size() != count) return output;
    for (auto &beam : output) beam.resize(count);
    for (int n = 0; n < count; ++n) {
        for (int ch : m_enabledChannels) m_ring[ch][m_position] = channels[ch][n];
        for (int beam = 0; beam < 3; ++beam) {
            double value = 0;
            for (int ch : m_enabledChannels) {
                const auto &tap = m_taps[beam][ch];
                for (int i = 0; i < 4; ++i) {
                    const int index = (m_position - tap.integer - i + m_ringSize) % m_ringSize;
                    value += tap.weights[i] * m_ring[ch][index];
                }
            }
            output[beam][n] = float(value / m_enabledChannels.size());
        }
        m_position = (m_position + 1) % m_ringSize;
    }
    return output;
}

void Denoiser::configure(const ProcessingSettings &settings)
{
    if (m_configured && onlyDirectionChange(m_settings, settings)) {
        m_beamformer.configure(settings.geometry, settings.directions, settings.soundSpeed, settings.disabledChannels);
        m_settings = settings;
        return;
    }
    m_settings = settings;
    m_beamformer.configure(settings.geometry, settings.directions, settings.soundSpeed, settings.disabledChannels);
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
