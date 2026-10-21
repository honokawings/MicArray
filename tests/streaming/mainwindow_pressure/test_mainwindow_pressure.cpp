#include "../test_helpers.h"
#include "media/noisereductionmailbox.h"
#include "ui/mainwindow.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTabWidget>
#include <chrono>
#include <mutex>
#include <thread>

class MainWindowPressureTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("MicArrayIndependentPressure");
        QCoreApplication::setApplicationName("MainWindowOffscreen20261021");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        qRegisterMetaType<HostFrame>("MicArray::HostFrame");
    }
    void sustainedAudio_data()
    {
        QTest::addColumn<bool>("debugVisible");
        QTest::newRow("debug_hidden") << false;
        QTest::newRow("debug_visible") << true;
    }
    void sustainedAudio()
    {
        QFETCH(bool, debugVisible);
        QTemporaryDir configDirectory;
        QVERIFY(configDirectory.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, configDirectory.path());
        { AppSettings settings; settings.saveNoiseReduction(settingsFor(3)); }
        MainWindow window;
        window.resize(1440, 900);
        auto *page = window.findChild<NoiseReductionPage *>("noiseReductionPage");
        auto *tabs = window.findChild<QTabWidget *>("mainTabs");
        auto *debug = window.findChild<QWidget *>("advancedFrameDebugPage");
        QVERIFY(page && tabs && debug);
        // This independent test has no socket and must never send controls to a device.
        QObject::disconnect(page, &NoiseReductionPage::controlSendRequested, &window, nullptr);
        tabs->setCurrentWidget(debugVisible ? debug : static_cast<QWidget *>(page));
        page->setRecordingRequired(true, {NoiseReductionDataFlowStage::AutomaticGainControl});
        Handshake handshake;
        connectHandshake(*page, 3, handshake);
        page->setControlLocked(false);
        page->setConnected(true);
        window.show(); // Offscreen platform: actual Qt widget paint, no user window.
        QTest::qWait(100);
        QVERIFY(page->m_plSession->ready());

        struct Output { std::mutex mutex; QVector<float> samples; };
        const auto actual = std::make_shared<Output>();
        QStringList warnings;
        connect(page->m_worker, &NoiseReductionWorker::recordingFrameReady, page->m_worker,
                [actual](const NoiseReductionRecordingFrame &frame) {
            std::lock_guard<std::mutex> guard(actual->mutex);
            for (const auto &stage : frame.stages)
                if (stage.stage == NoiseReductionDataFlowStage::AutomaticGainControl) actual->samples += stage.samples;
        }, Qt::DirectConnection);
        connect(page, &NoiseReductionPage::persistentPlDiagnostic, &window, [&](const QString &message) {
            if (message.contains("first-fault")) warnings.append(message);
        });
        constexpr int frames = 2400; // 480000 samples / 48000 Hz = ten real seconds.
        const quint16 frozenStream = handshake.stream, frozenActive = handshake.active;
        std::atomic<int> ingressed {0};
        std::atomic<bool> invocationFailed {false};
        std::atomic<qint64> maximumLatenessUs {0};
        const auto start = std::chrono::steady_clock::now();
        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
        watchdog.start(15000);
        std::thread producer([&] {
            for (int index = 0; index < frames; ++index) {
                const auto due = start + std::chrono::nanoseconds(qint64(index + 1) * 1000000000LL / 240);
                std::this_thread::sleep_until(due);
                const auto late = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - due).count();
                qint64 previous = maximumLatenessUs.load();
                while (late > previous && !maximumLatenessUs.compare_exchange_weak(previous, late)) {}
                HostFrame frame;
                frame.type = FrameType::Audio;
                frame.rawType = static_cast<quint16>(FrameType::Audio);
                frame.payload = audio200(3, quint32(index * 200), frozenStream);
                // Refresh real transport watchdog metadata without adding audio samples.
                const QByteArray status = statusPacket(3, frozenActive, frozenStream, frozenActive, 0x101);
                for (int byte = 0; byte < 41; ++byte) frame.payload[199 * 192 + (byte + 7) * 4 + 3] = status[byte];
                frame.payloadLength = frame.payload.size();
                frame.timestampSeconds = 10 + quint32(index / 240);
                frame.timestampMicros = quint32((qint64(index % 240) * 1000000) / 240);
                if (!QMetaObject::invokeMethod(&window, [&, frame] {
                    if (!QMetaObject::invokeMethod(&window, "enqueueAudioFrame", Qt::DirectConnection, Q_ARG(MicArray::HostFrame, frame))
                        || !QMetaObject::invokeMethod(&window, "updateDebugFrame", Qt::DirectConnection, Q_ARG(MicArray::HostFrame, frame))) invocationFailed = true;
                    if (++ingressed == frames) loop.quit();
                }, Qt::QueuedConnection)) invocationFailed = true;
            }
        });
        loop.exec();
        producer.join();
        const double wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        QCOMPARE(ingressed.load(), frames);
        QVERIFY(!invocationFailed.load());
        QTRY_COMPARE_WITH_TIMEOUT(page->m_mailbox->snapshot().plBufferedSamples, qint64(0), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(page->m_mailbox->snapshot().referenceBufferedSamples, qint64(0), 5000);
        QVector<float> output;
        { std::lock_guard<std::mutex> guard(actual->mutex); output = actual->samples; }
        QCOMPARE(output.size(), frames * 200);
        for (int sample = 0; sample < output.size(); ++sample) {
            const float expected = float(1 + quint32(sample) % 4093) / 65536.0f;
            QCOMPARE(std::bit_cast<quint32>(output[sample]), std::bit_cast<quint32>(expected));
        }
        const auto state = page->m_mailbox->snapshot();
        QCOMPARE(page->m_plFaultCount, quint64(0));
        QCOMPARE(state.wakeFailures, quint64(0));
        QVERIFY(warnings.isEmpty());
        const auto renders = window.property("debugFramesRendered").toULongLong();
        QCOMPARE(window.property("debugFramesReceived").toULongLong(), quint64(frames));
        if (debugVisible) { QVERIFY(renders > 0); QVERIFY(renders <= quint64(wallSeconds * 20 + 5)); }
        else QCOMPARE(renders, quint64(0));
        qInfo().noquote() << "[MAINWINDOW_PRESSURE_METRICS]" << QJsonDocument(QJsonObject {
            {"stage", 3}, {"debug_visible", debugVisible}, {"frames", frames}, {"samples", frames * 200},
            {"wall_seconds", wallSeconds}, {"nominal_samples_per_second", 48000},
            {"producer_max_lateness_us", maximumLatenessUs.load()}, {"pl_peak_samples", state.plPeakSamples},
            {"raw_peak_samples", state.referencePeakSamples}, {"debug_renders", qint64(renders)},
            {"actual_float_bits_compared", output.size()}, {"slot_completion_only", false},
            {"platform", QGuiApplication::platformName()}, {"scope", "real_MainWindow_Page_worker_offscreen_widget_render_no_socket_no_audio_backend"}
        }).toJson(QJsonDocument::Compact);
        QObject::disconnect(page->m_worker, nullptr, page->m_worker, nullptr);
    }
};
QTEST_MAIN(MainWindowPressureTest)
#include "test_mainwindow_pressure.moc"
