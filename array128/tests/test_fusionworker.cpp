#include "fusionworker.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <limits>

using namespace MicArray;
using namespace MicArray::Array128;

namespace {
FusionPacket packet(quint64 generation = 7, quint64 run = 3, quint64 frame = 1)
{
    FusionPacket value;
    value.video = QImage(320, 192, QImage::Format_RGB888); value.video.fill(QColor(40, 80, 120));
    value.heat = QVector<float>(72 * 45, .25f); value.columns = 72; value.rows = 45;
    value.generation = generation; value.run = run; value.frame = frame; value.ageMs = 0;
    return value;
}

FusionWorkerSettings actualSettings()
{
    const auto directory = qEnvironmentVariable("M128_FUSION_MODEL_DIR");
    FusionWorkerSettings settings;
    settings.enabled = true; settings.generation = 7; settings.run = 3;
    settings.modelPath = directory + "/fusion.onnx"; settings.metadataPath = directory + "/fusion.json";
    settings.runtimePath = qEnvironmentVariable("M128_ONNXRUNTIME");
    return settings;
}

bool hasRuntimeFixture()
{
    return !qEnvironmentVariable("M128_FUSION_MODEL_DIR").isEmpty() && !qEnvironmentVariable("M128_ONNXRUNTIME").isEmpty();
}

bool waitUntilBusy(FusionWorker &worker)
{
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 2000) {
        if (worker.snapshot().busy) return true;
        QThread::yieldCurrentThread();
    }
    return false;
}
}

class FusionWorkerTest : public QObject
{
    Q_OBJECT
private slots:
    void disabledByDefaultAndMissingModelIsExplicit()
    {
        FusionWorker worker;
        QVERIFY(!worker.snapshot().enabled); QVERIFY(!worker.snapshot().ready);
        QVERIFY(!worker.submit(packet())); QCOMPARE(worker.snapshot().queued, 0);
        FusionWorkerSettings settings; settings.enabled = true;
        settings.modelPath = "nonexistent-model.onnx"; settings.metadataPath = "nonexistent-model.json";
        const auto revision = worker.configure(settings);
        QVERIFY(revision > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!worker.snapshot().error.isEmpty(), 2000);
        QVERIFY(!worker.snapshot().ready); QVERIFY(!worker.snapshot().result.valid);
        QVERIFY(!worker.snapshot().status.isEmpty());
        settings.maxAgeMs = 0; worker.configure(settings);
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().error.contains(QStringLiteral("时序")), 2000);
        worker.disable();
        QVERIFY(!worker.snapshot().enabled); QVERIFY(!worker.snapshot().ready); QVERIFY(worker.snapshot().error.isEmpty());
        worker.shutdown(); QVERIFY(worker.waitForStopped(2000)); QVERIFY(worker.snapshot().stopped);
        const auto terminalRevision = worker.snapshot().revision;
        settings.enabled = true; worker.configure(settings);
        QCOMPARE(worker.snapshot().revision, terminalRevision); QVERIFY(!worker.submit(packet()));
    }

    void timingAndIdentityAreRejectedBeforeInference()
    {
        FusionWorker worker;
        FusionWorkerSettings settings; settings.enabled = true; settings.generation = 7; settings.run = 3;
        worker.configure(settings);
        auto value = packet();
        value.ageMs = -1; QVERIFY(!worker.submit(value));
        value.ageMs = 0; value.audioVideoSkewMs = 101; QVERIFY(!worker.submit(value));
        value.audioVideoSkewMs = -101; QVERIFY(!worker.submit(value));
        value.audioVideoSkewMs = std::numeric_limits<qint64>::min(); QVERIFY(!worker.submit(value));
        value.audioVideoSkewMs = 0; value.ageMs = 501; QVERIFY(!worker.submit(value));
        value.ageMs = 0; value.generation = 6; QVERIFY(!worker.submit(value));
        value.generation = 7; value.run = 2; QVERIFY(!worker.submit(value));
        QCOMPARE(worker.snapshot().rejected, quint64(7)); QCOMPARE(worker.snapshot().submitted, quint64(0));
        QCOMPARE(worker.snapshot().processed, quint64(0)); QCOMPARE(worker.snapshot().queued, 0);
    }

    void actualInferenceRevisionInvalidationAndBoundedQueue()
    {
        if (!hasRuntimeFixture()) {
            if (qEnvironmentVariableIntValue("M128_REQUIRE_FUSION_RUNTIME")) QFAIL("Actual ONNX runtime/model fixture is required");
            QSKIP("Set M128_FUSION_MODEL_DIR/M128_ONNXRUNTIME for the real background inference test");
        }
        FusionWorker worker;
        auto settings = actualSettings();
        worker.configure(settings);
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().ready || !worker.snapshot().error.isEmpty(), 10000);
        QVERIFY2(worker.snapshot().ready, qPrintable(worker.snapshot().error));
        QCOMPARE(worker.snapshot().metadata["format"].toString(), QString("micarray128-yolox-fusion-v1"));
        QVERIFY(!worker.snapshot().runtimeVersion.isEmpty());
        QVERIFY(worker.submit(packet()));
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().result.valid, 2000);
        QCOMPARE(worker.snapshot().generation, quint64(7)); QCOMPARE(worker.snapshot().run, quint64(3)); QCOMPARE(worker.snapshot().frame, quint64(1));
        QVERIFY(!worker.submit(packet())); // Do not recompute the same audio window on every UI timer tick.
        const auto oldRevision = worker.snapshot().revision;
        QVERIFY(worker.invalidate(8, 3) > oldRevision);
        QVERIFY(!worker.snapshot().result.valid); QCOMPARE(worker.snapshot().queued, 0);
        QVERIFY(!worker.submit(packet(7, 3, 2)));
        QVERIFY(worker.submit(packet(8, 3, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().result.valid && worker.snapshot().generation == 8, 2000);
        worker.invalidate(8, 4);
        QVERIFY(!worker.submit(packet(8, 3, 2)));
        QVERIFY(worker.submit(packet(8, 4, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().result.valid && worker.snapshot().run == 4, 2000);

        // A real inference, including 4K input conversion, is executing while
        // the main event loop remains responsive. Reconfiguration must prevent
        // its result from reappearing even when the native call finishes later.
        auto large = packet(8, 4, 2);
        large.video = QImage(4096, 2160, QImage::Format_ARGB32); large.video.fill(QColor(30, 90, 150));
        const auto discarded = worker.snapshot().discarded;
        QVERIFY(worker.submit(large)); QVERIFY(waitUntilBusy(worker));
        bool mainThreadEvent = false;
        QTimer::singleShot(0, this, [&] { mainThreadEvent = true; });
        QCoreApplication::processEvents(); QVERIFY(mainThreadEvent);
        const auto invalidated = worker.invalidate(9, 4);
        QVERIFY(!worker.snapshot().result.valid);
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().discarded > discarded, 3000);
        QCOMPARE(worker.snapshot().revision, invalidated); QVERIFY(!worker.snapshot().result.valid);

        const auto previousSubmitted = worker.snapshot().submitted;
        const auto previousReplaced = worker.snapshot().replaced;
        for (quint64 frame = 100; frame < 600; ++frame) {
            auto next = packet(9, 4, frame);
            QVERIFY(worker.submit(std::move(next)));
            QVERIFY(worker.snapshot().queued <= 1);
        }
        QCOMPARE(worker.snapshot().submitted - previousSubmitted, quint64(500));
        QVERIFY(worker.snapshot().replaced > previousReplaced);
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().result.valid && worker.snapshot().frame == 599, 3000);
        QCOMPARE(worker.snapshot().generation, quint64(9)); QCOMPARE(worker.snapshot().run, quint64(4));
        worker.disable();
        QVERIFY(!worker.snapshot().enabled); QVERIFY(!worker.snapshot().result.valid); QCOMPARE(worker.snapshot().queued, 0);
        QTest::qWait(50); QVERIFY(!worker.snapshot().result.valid);
        QVERIFY(!worker.submit(packet(9, 4, 600)));
    }

    void expiryAndShutdownWhileBusy()
    {
        if (!hasRuntimeFixture()) {
            if (qEnvironmentVariableIntValue("M128_REQUIRE_FUSION_RUNTIME")) QFAIL("Actual ONNX runtime/model fixture is required");
            QSKIP("Set actual ONNX fixture paths to test expiry and in-flight shutdown");
        }
        FusionWorker worker;
        auto settings = actualSettings(); settings.maxAgeMs = 200;
        worker.configure(settings);
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().ready || !worker.snapshot().error.isEmpty(), 10000);
        QVERIFY2(worker.snapshot().ready, qPrintable(worker.snapshot().error));
        QVERIFY(worker.submit(packet()));
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().result.valid, 2000);
        QTest::qWait(220);
        QVERIFY(!worker.snapshot().result.valid); QVERIFY(worker.snapshot().ageMs > 200);
        QVERIFY(worker.snapshot().status.contains(QStringLiteral("过期")));
        const auto rejected = worker.snapshot().rejected;
        auto expiresDuringProcessing = packet(7, 3, 2); expiresDuringProcessing.ageMs = 200;
        QVERIFY(worker.submit(expiresDuringProcessing));
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().rejected > rejected, 2000);
        QVERIFY(!worker.snapshot().result.valid);
        auto large = packet(7, 3, 3);
        large.video = QImage(4096, 2160, QImage::Format_ARGB32); large.video.fill(Qt::darkGray);
        QVERIFY(worker.submit(large)); QVERIFY(waitUntilBusy(worker));
        QElapsedTimer timer; timer.start(); worker.shutdown();
        QVERIFY2(timer.elapsed() < 100, "shutdown blocked on background inference");
        QVERIFY(!worker.snapshot().enabled); QVERIFY(!worker.snapshot().result.valid);
        QVERIFY(worker.waitForStopped(5000)); QVERIFY(worker.snapshot().stopped);
        QCOMPARE(worker.snapshot().queued, 0); QVERIFY(!worker.snapshot().result.valid);
    }

    void configureReloadsSamePathButInvalidatePreservesSession()
    {
        if (!hasRuntimeFixture()) {
            if (qEnvironmentVariableIntValue("M128_REQUIRE_FUSION_RUNTIME")) QFAIL("Actual ONNX runtime/model fixture is required");
            QSKIP("Set actual ONNX fixture paths for same-path reload validation");
        }
        QTemporaryDir directory; QVERIFY(directory.isValid());
        auto settings = actualSettings();
        const QString modelPath = directory.path() + "/fusion.onnx", metadataPath = directory.path() + "/fusion.json";
        QVERIFY(QFile::copy(settings.modelPath, modelPath)); QVERIFY(QFile::copy(settings.metadataPath, metadataPath));
        settings.modelPath = modelPath; settings.metadataPath = metadataPath;
        FusionWorker worker; worker.configure(settings);
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().ready || !worker.snapshot().error.isEmpty(), 10000);
        QVERIFY2(worker.snapshot().ready, qPrintable(worker.snapshot().error));
        QFile metadata(metadataPath); QVERIFY(metadata.open(QIODevice::ReadOnly));
        auto description = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
        description["model_sha256"] = QString(64, '0');
        QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
        metadata.write(QJsonDocument(description).toJson()); metadata.close();
        worker.invalidate(8, 3); // A new frame/geometry context must not re-read unchanged model paths.
        QVERIFY(worker.submit(packet(8, 3, 1)));
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().result.valid, 2000);
        settings.generation = 8;
        worker.configure(settings); // User explicitly reloads: same path now has a bad hash.
        QVERIFY(!worker.snapshot().result.valid);
        QTRY_VERIFY_WITH_TIMEOUT(worker.snapshot().error.contains("SHA-256"), 5000);
        QVERIFY(!worker.snapshot().ready); QVERIFY(!worker.snapshot().result.valid);
    }
};

QTEST_GUILESS_MAIN(FusionWorkerTest)
#include "test_fusionworker.moc"
