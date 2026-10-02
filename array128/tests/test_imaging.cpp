#include "dsp.h"
#include "localizationfft.h"
#include "pipeline.h"
#include "analysis/directionprojection.h"
#include "analysis/radix2fft.h"

#include <QTest>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QtEndian>
#include <QJsonDocument>
#include <QFile>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

using namespace MicArray;
using namespace MicArray::Array128;

namespace {
constexpr double Pi = 3.14159265358979323846;

Geometry testSpiral(bool nonplanar = false)
{
    Geometry geometry;
    geometry.name = "SYNTHETIC TEST ONLY - not a manufacturing coordinate table";
    for (int ch = 0; ch < 128; ++ch) {
        const double radius = .015 * std::sqrt(ch + 1.0), theta = ch * 2.399963229728653;
        geometry.microphones.append({radius * std::cos(theta), radius * std::sin(theta), nonplanar ? .025 * std::sin(ch * .7) : 0});
    }
    return geometry;
}

// Independent forward model: compute Euclidean source-to-microphone distance.
// The common 100 m flight time is removed only to avoid a long initial silence.
// A microphone nearer the source receives it EARLIER. No steering delays or
// projection code from the implementation under test is used to generate PCM.
QVector<QVector<float>> sphericalSource(const Geometry &geometry, double azimuth, double elevation,
                                      double snr = 100, int tones = 32, double speed = 343, double range = 100, double frequencyOffset = 0)
{
    const double az = azimuth * Pi / 180, el = elevation * Pi / 180;
    const double sx = range * std::sin(az) * std::cos(el), sy = range * std::sin(el), sz = range * std::cos(az) * std::cos(el);
    QVector<QVector<float>> audio(128, QVector<float>(2048));
    std::mt19937 rng(0x128abc);
    std::normal_distribution<double> noise(0, .008 * std::sqrt(tones / 2.0) * std::pow(10, -snr / 20));
    for (int ch = 0; ch < 128; ++ch) {
        const auto &p = geometry.microphones[ch];
        const double distance = std::sqrt(std::pow(sx - p.xMeters, 2) + std::pow(sy - p.yMeters, 2) + std::pow(sz - p.zMeters, 2));
        const double propagationSeconds = (distance - range) / speed;
        for (int n = 0; n < 2048; ++n) {
            double value = noise(rng);
            for (int k = 0; k < tones; ++k)
                value += .008 * std::sin(2 * Pi * (373 + k * 197 + frequencyOffset) * (n / 48000.0 - propagationSeconds) + k * 1.713);
            audio[ch][n] = float(value);
        }
    }
    return audio;
}

double angularError(const ScanDirection &actual, double azimuth, double elevation)
{
    return std::hypot(actual.azimuthDegrees - azimuth, actual.elevationDegrees - elevation);
}
}

class ImagingTest : public QObject
{
    Q_OBJECT
private slots:
    void plannedFftMatchesReference()
    {
        QVector<std::complex<double>> input(2048);
        std::mt19937 rng(7234); std::normal_distribution<double> sample;
        for (auto &value : input) value = {sample(rng), sample(rng)};
        auto expected = input, actual = input;
        QVERIFY(Radix2Fft::transform(&expected, false)); QVERIFY(LocalizationFft::transform(&actual, false));
        for (int i = 0; i < input.size(); ++i) QVERIFY(std::abs(expected[i] - actual[i]) < 1e-8);
        QVERIFY(LocalizationFft::transform(&actual, true));
        for (int i = 0; i < input.size(); ++i) QVERIFY(std::abs(input[i] - actual[i]) < 1e-10);
        QVERIFY(!LocalizationFft::transform(nullptr, false));
        QVector<std::complex<double>> shortInput(1024); QVERIFY(!LocalizationFft::transform(&shortInput, false));
    }
    void physicalDirection_data()
    {
        QTest::addColumn<double>("az"); QTest::addColumn<double>("el"); QTest::addColumn<double>("snr");
        for (double az : {-35.0, 0.0, 35.0}) for (double el : {-20.0, 0.0, 20.0})
            for (double snr : {30.0, 0.0})
                QTest::newRow(qPrintable(QString("az%1-el%2-snr%3").arg(az).arg(el).arg(snr))) << az << el << snr;
    }
    void physicalDirection()
    {
        QFETCH(double, az); QFETCH(double, el); QFETCH(double, snr);
        ProcessingSettings settings; settings.geometry = testSpiral();
        const auto result = localize(sphericalSource(settings.geometry, az, el, snr), settings);
        QVERIFY2(result.trusted, qPrintable(result.reason + QString::number(result.confidence)));
        QVERIFY2(angularError(result.direction, az, el) < 2,
            qPrintable(QString("actual=%1/%2 expected=%3/%4 confidence=%5").arg(result.direction.azimuthDegrees).arg(result.direction.elevationDegrees).arg(az).arg(el).arg(result.confidence)));
        const int index = int(std::max_element(result.heat.cbegin(), result.heat.cend()) - result.heat.cbegin());
        const auto heatDirection = unprojectVideoPoint({double(index % result.columns) / (result.columns - 1), double(index / result.columns) / (result.rows - 1)}, settings.camera);
        QVERIFY(angularError(heatDirection, az, el) < 3);
        for (float value : result.heat) QVERIFY(std::isfinite(value) && value >= 0 && value <= 1);
    }
    void beamPointsTowardPhysicalSource()
    {
        const auto geometry = testSpiral(); const auto audio = sphericalSource(geometry, 25, -12);
        Beamformer beamformer; beamformer.configure(geometry, {{{25, -12}, {-25, 12}, {0, 0}}}, 343);
        const auto output = beamformer.process(audio);
        std::array<double, 3> power {};
        for (int beam = 0; beam < 3; ++beam) for (int n = 1024; n < 2048; ++n) power[beam] += output[beam][n] * output[beam][n];
        // Low frequencies have a wide main lobe on a 34 cm aperture: a fixed
        // 10 dB rejection at every off-axis direction is not a physical contract.
        // Instead require the target beam to retain >=90% of coherent-source
        // power and exceed both off-axis beams; the mirror must be >10 dB lower.
        Geometry origin; origin.microphones.resize(128);
        const auto reference = sphericalSource(origin, 25, -12);
        double referencePower = 0;
        for (int n = 1024; n < 2048; ++n) referencePower += reference[0][n] * reference[0][n];
        QVERIFY(power[0] > referencePower * .90);
        QVERIFY2(power[0] > power[1] * 10 && power[0] > power[2],
            qPrintable(QString("target=%1 mirror=%2 broadside=%3").arg(power[0]).arg(power[1]).arg(power[2])));
    }
    void geometryMappingAndCamera()
    {
        ProcessingSettings settings; settings.geometry = testSpiral(true);
        auto audio = sphericalSource(settings.geometry, 24.3, -9.2, 10);
        std::reverse(settings.geometry.microphones.begin(), settings.geometry.microphones.end());
        std::reverse(audio.begin(), audio.end());
        settings.camera = {15, -5, 70, 50, 17};
        const auto result = localize(audio, settings);
        QVERIFY(result.trusted); QVERIFY(angularError(result.direction, 24.3, -9.2) < 2);
        SoundSourceDirection direction; direction.azimuthDegrees = result.direction.azimuthDegrees; direction.elevationDegrees = result.direction.elevationDegrees;
        QVERIFY(projectDirection(direction, settings.camera).inView);
    }
    void invalidAndUninformativeInputs()
    {
        ProcessingSettings settings; settings.geometry = testSpiral();
        QVector<QVector<float>> silence(128, QVector<float>(2048, 0));
        QVERIFY(!localize(silence, settings).trusted);
        for (auto &channel : silence) channel.fill(.25f);
        QVERIFY(!localize(silence, settings).trusted);
        QVERIFY(!localize(sphericalSource(settings.geometry, 15, 0, 100, 1), settings).trusted);
        auto audio = sphericalSource(settings.geometry, 15, 0);
        auto invalid = settings; invalid.soundSpeed = 0;
        QVERIFY(localize(audio, invalid).heat.isEmpty());
        invalid = settings; invalid.camera.horizontalFovDegrees = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(localize(audio, invalid).heat.isEmpty());
        invalid = settings; invalid.geometry.microphones[3].xMeters = std::numeric_limits<double>::infinity();
        QVERIFY(localize(audio, invalid).heat.isEmpty());
        audio[4].resize(100); QVERIFY(localize(audio, settings).heat.isEmpty());
    }
    void damagedChannelsAndDc()
    {
        ProcessingSettings settings; settings.geometry = testSpiral();
        auto audio = sphericalSource(settings.geometry, -17.4, 11.2, 10);
        for (int ch = 0; ch < 128; ++ch) for (auto &sample : audio[ch]) sample = sample * (.7f + (ch % 7) * .1f) + .15f;
        for (int ch = 0; ch < 16; ++ch) audio[ch * 7 % 128].fill(0);
        audio[125].fill(std::numeric_limits<float>::quiet_NaN());
        const auto result = localize(audio, settings);
        QVERIFY(result.trusted); QVERIFY(angularError(result.direction, -17.4, 11.2) < 2);
        QCOMPARE(result.validChannels, 111);
    }
    void independentNoiseAndThreshold()
    {
        ProcessingSettings settings; settings.geometry = testSpiral();
        QVector<QVector<float>> noise(128, QVector<float>(2048));
        std::mt19937 rng(73621); std::normal_distribution<float> sample(0, .03f);
        for (int run = 0; run < 8; ++run) {
            for (auto &channel : noise) for (auto &value : channel) value = sample(rng);
            const auto result = localize(noise, settings);
            QVERIFY2(!result.trusted, qPrintable(QString("Uncorrelated noise confidence=%1").arg(result.confidence)));
        }
        auto audio = sphericalSource(settings.geometry, 15, -10, 0);
        const auto baseline = localize(audio, settings); QVERIFY(baseline.trusted);
        settings.confidenceThreshold = 1;
        const auto rejected = localize(audio, settings);
        QVERIFY(!rejected.trusted); QCOMPARE(rejected.direction, baseline.direction); QCOMPARE(rejected.heat, baseline.heat);
        settings.confidenceThreshold = 0;
        QVERIFY(localize(audio, settings).trusted);
        for (auto &channel : audio) channel.fill(0);
        QVERIFY(!localize(audio, settings).trusted); // Threshold must not bypass silence protection.
    }
    void wideViewAndGridIndependence_data()
    {
        QTest::addColumn<int>("columns"); QTest::addColumn<int>("rows");
        QTest::addColumn<double>("az"); QTest::addColumn<double>("el");
        for (const auto grid : {QSize(24, 15), QSize(72, 45), QSize(160, 100)})
            for (const auto point : {QPointF(0, 0), QPointF(13.25, -8.5), QPointF(65, 20)})
                QTest::newRow(qPrintable(QString("%1x%2-%3-%4").arg(grid.width()).arg(grid.height()).arg(point.x()).arg(point.y())))
                    << grid.width() << grid.height() << point.x() << point.y();
    }
    void wideViewAndGridIndependence()
    {
        QFETCH(int, columns); QFETCH(int, rows); QFETCH(double, az); QFETCH(double, el);
        ProcessingSettings settings; settings.geometry = testSpiral();
        settings.camera = {0, 0, 170, 120, 0}; settings.gridColumns = columns; settings.gridRows = rows;
        const auto result = localize(sphericalSource(settings.geometry, az, el, 20), settings);
        QVERIFY2(result.trusted, qPrintable(result.reason + QString::number(result.confidence)));
        QVERIFY2(angularError(result.direction, az, el) < 2,
            qPrintable(QString("actual=%1/%2 expected=%3/%4").arg(result.direction.azimuthDegrees).arg(result.direction.elevationDegrees).arg(az).arg(el)));
        SoundSourceDirection actual; actual.azimuthDegrees = az; actual.elevationDegrees = el;
        const auto expected = projectDirection(actual, settings.camera).normalizedPoint;
        const auto peak = std::max_element(result.heat.cbegin(), result.heat.cend());
        const int index = int(peak - result.heat.cbegin());
        QVERIFY(*peak > .99f);
        QVERIFY(std::abs(index % columns - expected.x() * (columns - 1)) < 1.5);
        QVERIFY(std::abs(index / columns - expected.y() * (rows - 1)) < 1.5);
    }
    void maximumGridPerformance()
    {
        ProcessingSettings settings; settings.geometry = testSpiral(); settings.gridColumns = 160; settings.gridRows = 100;
        const auto audio = sphericalSource(settings.geometry, 13.25, -8.5);
        QVector<double> durations;
        for (int run = 0; run < 5; ++run) {
            const auto result = localize(audio, settings); QVERIFY(result.trusted);
            QCOMPARE(result.heat.size(), 16000); QVERIFY(angularError(result.direction, 13.25, -8.5) < 2);
            durations.append(result.elapsedMs);
        }
        std::sort(durations.begin(), durations.end());
        qInfo() << "128 channels / 160x100 grid median ms:" << durations[2] << "max ms:" << durations.back();
        QVERIFY2(durations[2] < 100, "Maximum-grid median exceeds 10 FPS CPU budget on this test host");
    }
    void nearFieldAndAperture_data()
    {
        QTest::addColumn<double>("range"); QTest::addColumn<double>("scale");
        QTest::newRow("near-0.7m") << .7 << 1.0;
        QTest::newRow("near-1.5m") << 1.5 << 1.0;
        QTest::newRow("room-5m") << 5.0 << 1.0;
        QTest::newRow("aperture-85cm") << 100.0 << 2.5;
    }
    void nearFieldAndAperture()
    {
        QFETCH(double, range); QFETCH(double, scale);
        ProcessingSettings settings; settings.geometry = testSpiral(); settings.camera = {0, 0, 170, 120, 0};
        for (auto &p : settings.geometry.microphones) { p.xMeters *= scale; p.yMeters *= scale; }
        const auto result = localize(sphericalSource(settings.geometry, 23.4, -11.3, 20, 32, 343, range), settings);
        QVERIFY2(result.trusted, qPrintable(result.reason));
        QVERIFY2(angularError(result.direction, 23.4, -11.3) < 3,
            qPrintable(QString("range %1m actual %2/%3").arg(range).arg(result.direction.azimuthDegrees).arg(result.direction.elevationDegrees)));
    }
    void competingSources()
    {
        ProcessingSettings settings; settings.geometry = testSpiral();
        auto signal = sphericalSource(settings.geometry, 23, -12, 30);
        const auto interferer = sphericalSource(settings.geometry, -28, 15, 30, 32, 343, 100, 83);
        for (int ch = 0; ch < Channels; ++ch) for (int n = 0; n < 2048; ++n) signal[ch][n] += interferer[ch][n] * .45f;
        const auto result = localize(signal, settings);
        QVERIFY(result.trusted); QVERIFY(angularError(result.direction, 23, -12) < 3);
    }
    void outOfViewIsNotAFalseEdgeSource()
    {
        ProcessingSettings settings; settings.geometry = testSpiral(); settings.camera = {0, 0, 40, 30, 0};
        const auto result = localize(sphericalSource(settings.geometry, 38, 0, 30), settings);
        QVERIFY2(!result.trusted, qPrintable(QString("False in-view source %1/%2 confidence %3").arg(result.direction.azimuthDegrees).arg(result.direction.elevationDegrees).arg(result.confidence)));
    }
    void acousticUdpWithConcurrentDspAndRecording()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        ProcessingSettings settings; settings.geometry = testSpiral(); settings.localization = true; settings.denoise = true;
        settings.gridColumns = 160; settings.gridRows = 100; settings.monitor = true; settings.monitorSource = 7;
        settings.bandpass = true; settings.logmmse = true; settings.agc = true;
        settings.directions = {{{13.25, -8.5}, {-30, 0}, {30, 0}}}; settings.generation = 1;
        const auto audio = sphericalSource(settings.geometry, 13.25, -8.5, 30);
        QByteArray prototype(1208, 0); prototype.replace(0, 8, QByteArray::fromHex("4d31323801200180"));
        qToBigEndian<quint32>(48000, prototype.data() + 8); qToBigEndian<quint16>(3, prototype.data() + 12); qToBigEndian<quint16>(392, prototype.data() + 14);
        auto datagram = [&](quint64 sequence, quint32 epoch = 1, quint32 flags = 0) {
            auto bytes = prototype; qToBigEndian(sequence, bytes.data() + 16); qToBigEndian(epoch, bytes.data() + 24); qToBigEndian(flags, bytes.data() + 28);
            for (int r = 0; r < 3; ++r) {
                const quint64 frame = sequence * 3 + r; qToBigEndian(frame, bytes.data() + 32 + r * 392);
                for (int ch = 0; ch < Channels; ++ch) {
                    const quint32 value = quint32(qint32(std::lround(audio[ch][frame % 2048] * 8388608)));
                    char *p = bytes.data() + 40 + r * 392 + ch * 3;
                    p[0] = char(value); p[1] = char(value >> 8); p[2] = char(value >> 16);
                }
            }
            return bytes;
        };
        QUdpSocket sender, reservation; QVERIFY(sender.bind(QHostAddress::LocalHost, 0)); QVERIFY(reservation.bind(QHostAddress::LocalHost, 0));
        const auto port = reservation.localPort(); reservation.close();
        Pipeline pipeline; pipeline.configure(settings); pipeline.start({"127.0.0.1", port, "127.0.0.1", sender.localPort()});
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.snapshot().listening, 2000);
        RecordingSettings recording; recording.directory = directory.path(); recording.arraySource = false; recording.tracks = {0, 1, 2, 3};
        QVERIFY(pipeline.recorder().begin(recording));
        QElapsedTimer timer; timer.start(); quint64 sent = 0, previousFrame = 0; QVector<double> durations; qint64 lastPoll = -100;
        qsizetype monitorBytes = 0;
        const int seconds = std::clamp(qEnvironmentVariableIntValue("M128_STRESS_SECONDS"), 2, 30);
        const quint64 packetCount = quint64(seconds) * 16000; // Real 16,000 datagrams/s; optional bounded 30-second soak.
        while (sent < packetCount) {
            const quint64 due = std::min(packetCount, quint64(timer.nsecsElapsed() / 62500));
            while (sent < due) { const auto bytes = datagram(sent++); QCOMPARE(sender.writeDatagram(bytes, QHostAddress::LocalHost, port), qint64(bytes.size())); }
            if (timer.elapsed() - lastPoll >= 20) {
                lastPoll = timer.elapsed(); monitorBytes += pipeline.takeMonitorPcm().size();
                const auto state = pipeline.snapshot();
                if (state.localization.lastFrame != previousFrame && state.localization.trusted) {
                    previousFrame = state.localization.lastFrame; durations.append(state.localization.elapsedMs);
                    QVERIFY(angularError(state.localization.direction, 13.25, -8.5) < 3);
                }
            }
            QTest::qWait(1);
        }
        QTRY_COMPARE_WITH_TIMEOUT(pipeline.snapshot().valid, packetCount, 3000);
        auto state = pipeline.snapshot();
        QCOMPARE(state.queueDrops, 0u); QCOMPARE(state.discontinuities, 0u); QCOMPARE(state.invalid, 0u);
        QVERIFY2(durations.size() >= seconds * 6, qPrintable(QString("Only %1 trusted updates in %2 seconds").arg(durations.size()).arg(seconds)));
        std::sort(durations.begin(), durations.end());
        qInfo() << "Concurrent UDP + 128-beam + all postprocessors + four WAVs + 160x100: updates" << durations.size()
                << "median" << durations[durations.size() / 2] << "ms max" << durations.back() << "ms delay" << state.processingDelayMs;
        QVERIFY(durations[durations.size() / 2] < 100); QVERIFY(state.processingDelayMs < 250); QVERIFY(monitorBytes > 48000);
        // A quality fault in the same epoch must quarantine subsequent acoustics.
        QCOMPARE(sender.writeDatagram(datagram(sent++, 1, 1), QHostAddress::LocalHost, port), qint64(1208));
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.snapshot().latchedFlags != 0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.snapshot().localization.heat.isEmpty(), 2000);
        auto resumeAcoustics = [&] {
            for (int i = 0; i < 2200; ++i) {
                QCOMPARE(sender.writeDatagram(datagram(sent++, 2), QHostAddress::LocalHost, port), qint64(1208));
                if (i % 40 == 0) QTest::qWait(3);
            }
        };
        resumeAcoustics();
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.snapshot().localization.trusted, 1000);
        QCOMPARE(pipeline.snapshot().latchedFlags, 0u);
        // Changing configuration invalidates old pending/in-flight results, even
        // without receiving another packet to drive the processing thread.
        ++settings.generation; settings.camera.rollDegrees = 3; pipeline.configure(settings);
        QVERIFY(pipeline.snapshot().localization.heat.isEmpty()); QTest::qWait(150);
        QVERIFY(pipeline.snapshot().localization.heat.isEmpty()); QVERIFY(pipeline.takeMonitorPcm().isEmpty());
        resumeAcoustics();
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.snapshot().localization.trusted, 1000);
        QCOMPARE(pipeline.snapshot().localization.generation, settings.generation);
        // Deliberately skip one packet, then verify no previous heat survives.
        ++sent; QCOMPARE(sender.writeDatagram(datagram(sent++, 2), QHostAddress::LocalHost, port), qint64(1208));
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.snapshot().discontinuities > 0, 1000);
        QTest::qWait(150); QVERIFY(pipeline.snapshot().localization.heat.isEmpty());
        resumeAcoustics(); QTRY_VERIFY_WITH_TIMEOUT(pipeline.snapshot().localization.trusted, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.snapshot().localization.heat.isEmpty(), 2500); // No new packets: timeout.
        QVERIFY(pipeline.takeMonitorPcm().isEmpty());
        pipeline.stop(); QVERIFY(pipeline.snapshot().localization.heat.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!pipeline.recorder().snapshot().finishing, 3000);
        const auto recorded = pipeline.recorder().snapshot(); QVERIFY(recorded.error.isEmpty()); QCOMPARE(recorded.dropped, 0u); QVERIFY(recorded.frames > 80000);
        QFile manifest(recorded.directory + "/manifest.json"); QVERIFY(manifest.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(manifest.readAll()).object()["complete"].toBool());
        // Restart cannot publish an in-flight result from the previous run.
        pipeline.start({"127.0.0.1", port, "127.0.0.1", sender.localPort()});
        QTest::qWait(200); QVERIFY(pipeline.snapshot().localization.heat.isEmpty()); QVERIFY(pipeline.takeMonitorPcm().isEmpty()); pipeline.stop();
    }
};

int main(int argc, char **argv)
{
    Q_INIT_RESOURCE(reference);
    QCoreApplication app(argc, argv);
    ImagingTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_imaging.moc"
