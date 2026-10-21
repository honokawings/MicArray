#include "analysis/calibrationprofile.h"
#include "analysis/cameraviewcalibrator.h"
#include "analysis/directionprojection.h"
#include "analysis/plspatialprojector.h"
#include "app/appsettings.h"
#include "ui/livelocalizationpage.h"

#include <QCheckBox>
#include <QFile>
#include <QFontDatabase>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineF>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QtTest>

#include <algorithm>
#include <cmath>

using namespace MicArray;

namespace {
CameraViewSettings lensCamera()
{
    CameraViewSettings camera;
    camera.lens = {true, 0.45, 0.80, 0.48, 0.51, {-0.01, 0.0, 0.0, 0.0}, 1920, 1080};
    return camera;
}

QString writeLens(const QTemporaryDir &directory, const CameraLensCalibration &lens)
{
    const QString path = directory.filePath(QStringLiteral("lens.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { return {}; }
    const QJsonObject root {
        {QStringLiteral("format"), QStringLiteral("MicArrayCameraLens")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("lens"), cameraLensToJson(lens)},
    };
    file.write(QJsonDocument(root).toJson());
    return path;
}
}

class TestCameraLens : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        const int id = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) { qApp->setFont(QFont(families.first(), 9)); }
    }

    void agreesWithIndependentOpenCvReference()
    {
        QFile file(QStringLiteral(MICARRAY_LENS_FIXTURE));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
        CameraViewSettings camera;
        QVERIFY(cameraLensFromJson(fixture["lens"].toObject(), &camera.lens));
        const auto cases = fixture["cases"].toArray();
        QCOMPARE(cases.size(), 9);
        for (const auto &entry : cases)
        {
            const auto test = entry.toObject();
            const auto pixel = test["normalizedPixel"].toArray();
            const auto actual = projectDirection({test["azimuth"].toDouble(), test["elevation"].toDouble()}, camera);
            QVERIFY(actual.valid && actual.inFront);
            QVERIFY(QLineF(actual.normalizedPoint, {pixel[0].toDouble(), pixel[1].toDouble()}).length() < 2.0e-6);
        }
    }
    void knownFisheyeRays()
    {
        auto camera = lensCamera();
        camera.lens.coefficients = {};
        const auto center = projectDirection({0, 0}, camera);
        QVERIFY(QLineF(center.normalizedPoint, {.48, .51}).length() < 1.0e-7);
        // Equidistant model: 60 degrees -> x = cx + fx*pi/3.
        const auto right = projectDirection({60, 0}, camera);
        QVERIFY(right.inView);
        QVERIFY(std::abs(right.normalizedPoint.x() - (.48 + .45 * M_PI / 3)) < 1.0e-7);
        const auto above = projectDirection({0, 30}, camera);
        QVERIFY(std::abs(above.normalizedPoint.y() - (.51 - .80 * M_PI / 6)) < 1.0e-7);
        QVERIFY(!projectDirection({180, 0}, camera).inFront);
        QVERIFY(!projectDirection({89, 0}, camera).inView);
    }

    void entireViewportRoundTripsWithPose()
    {
        auto camera = lensCamera();
        camera.centerAzimuthDegrees = 17.0;
        camera.centerElevationDegrees = -8.0;
        camera.rollDegrees = 11.0;
        for (int row = 0; row <= 8; ++row)
        {
            for (int column = 0; column <= 12; ++column)
            {
                const QPointF pixel(column / 12.0, row / 8.0);
                const auto ray = unprojectVideoPoint(pixel, camera);
                const auto projected = projectDirection({ray.azimuthDegrees, ray.elevationDegrees}, camera);
                QVERIFY(projected.valid && projected.inFront);
                QVERIFY(QLineF(pixel, projected.normalizedPoint).length() < 2.0e-6);
                const auto angularRay = unprojectVideoPointAngular(pixel, camera);
                const auto angular = projectDirectionAngular({angularRay.azimuthDegrees, angularRay.elevationDegrees}, camera);
                QVERIFY(QLineF(pixel, angular.normalizedPoint).length() < 2.0e-6);
            }
        }
    }

    void invalidModelsAreRejected()
    {
        auto camera = lensCamera();
        camera.lens.coefficients[0] = -0.5;
        QVERIFY(!camera.isValid());
        QVERIFY(!projectDirection({0, 0}, camera).valid);
        QVERIFY(!std::isfinite(unprojectVideoPoint({.5, .5}, camera).azimuthDegrees));
        camera = lensCamera();
        camera.lens.fx = 0.0;
        QVERIFY(!camera.isValid());
        camera = lensCamera();
        camera.lens.enabled = false;
        camera.lens.fx = 0.0;
        QVERIFY(camera.isValid());
        QCOMPARE(projectDirection({0, 0}, camera).normalizedPoint, QPointF(.5, .5));
    }

    void settingsAndProfilesPreserveLensAndOldProfiles()
    {
        QTemporaryDir directory;
        const QString settingsPath = directory.filePath("settings.ini");
        auto camera = lensCamera();
        camera.lens.enabled = false;
        {
            AppSettings settings(settingsPath);
            auto live = settings.liveLocalization();
            live.camera = camera;
            settings.saveLiveLocalization(live);
        }
        AppSettings reloaded(settingsPath);
        QCOMPARE(reloaded.liveLocalization().camera.lens, camera.lens);
        VideoCalibrationProfile profile;
        profile.camera = camera;
        profile.savedAtUtc = QDateTime::currentDateTimeUtc();
        profile.referenceVideoSize = {1920, 1080};
        const auto path = directory.filePath("profile.json");
        QString error;
        QVERIFY2(VideoCalibrationProfileStore::save(path, profile, &error), qPrintable(error));
        VideoCalibrationProfile loaded;
        QVERIFY2(VideoCalibrationProfileStore::load(path, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.camera.lens, camera.lens);
        profile.camera = {};
        QVERIFY(VideoCalibrationProfileStore::save(path, profile, &error));
        QVERIFY(VideoCalibrationProfileStore::load(path, &loaded, &error));
        QVERIFY(!loaded.camera.lens.enabled);
        QVERIFY(!loaded.camera.lens.isValid());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        root["version"] = 2; // Missing v2 lens must not silently fall back.
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson());
        file.close();
        QVERIFY(!VideoCalibrationProfileStore::load(path, &loaded, &error));
    }

    void orientationFitKeepsIntrinsicsFixed()
    {
        auto expected = lensCamera();
        expected.centerAzimuthDegrees = 5;
        expected.centerElevationDegrees = -3;
        expected.rollDegrees = 4;
        QVector<VideoCalibrationSample> samples;
        for (const ScanDirection ray : {ScanDirection {-35, -18}, {30, -15}, {-28, 20}, {32, 17}})
        {
            VideoCalibrationSample sample;
            sample.direction = ray;
            sample.confidence = .9;
            sample.normalizedVideoPoint = projectDirection({ray.azimuthDegrees, ray.elevationDegrees}, expected).normalizedPoint;
            samples.append(sample);
        }
        auto initial = lensCamera();
        const auto fitted = fitCameraView(samples, initial, {1920, 1080});
        QVERIFY2(fitted.valid, qPrintable(fitted.error));
        QCOMPARE(fitted.camera.lens, initial.lens);
        QVERIFY(fitted.rmsPixels < .1);
    }

    void plHeatmapMatchesMarkersAndInvalidatesCache()
    {
        PlSpatialMap map;
        map.sourceMask = 1;
        map.values.fill(0.0f, 1369);
        map.values[21 * 37 + 29] = 1.0f; // az55, el15
        PlSpatialProjector projector;
        auto camera = lensCamera();
        for (const double cx : {.48, .42})
        {
            camera.lens.cx = cx;
            const auto response = projector.project(map, camera, 1);
            const auto maximum = std::max_element(response.normalizedValues.cbegin(), response.normalizedValues.cend());
            const int index = int(maximum - response.normalizedValues.cbegin());
            const QPointF pixel((index % response.columns + .5) / response.columns,
                                 (index / response.columns + .5) / response.rows);
            QVERIFY(QLineF(pixel, projectDirection({55, 15}, camera).normalizedPoint).length() < .025);
        }
    }

    void lensImportNativeFrameAndAspectGuard()
    {
        QTemporaryDir directory;
        const QString settingsPath = directory.filePath("ui.ini");
        AppSettings settings(settingsPath);
        auto config = settings.liveLocalization();
        config.camera.centerAzimuthDegrees = 12.0;
        settings.saveLiveLocalization(config);
        {
            LiveLocalizationPage page(&settings);
            QImage frame(1280, 720, QImage::Format_RGB32);
            frame.fill(QColor(13, 77, 42));
            page.setVideoFrame(frame, 1, 0);
            QString error;
            QVERIFY(page.loadLensCalibration(writeLens(directory, lensCamera().lens), &error));
            QCOMPARE(page.cameraView().centerAzimuthDegrees, 12.0);
            QVERIFY(page.cameraView().lens.enabled);
            const auto rawPath = directory.filePath("raw.png");
            QVERIFY(page.saveRawVideoFrame(rawPath, &error));
            QCOMPARE(QImage(rawPath), frame);
            auto *check = page.findChild<QCheckBox *>("cameraLensEnabledCheck");
            QVERIFY(check);
            check->setChecked(false);
            QVERIFY(!page.cameraView().lens.enabled);
            check->setChecked(true);
            QVERIFY(page.cameraView().lens.enabled);
            const auto qa = qEnvironmentVariable("MICARRAY_QA_DIR");
            if (!qa.isEmpty())
            {
                QDir().mkpath(qa);
                page.resize(1400, 900);
                auto *tabs = page.findChild<QTabWidget *>();
                QVERIFY(tabs);
                tabs->setCurrentIndex(1);
                page.show();
                QTest::qWait(80);
                QVERIFY(page.grab().save(QDir(qa).filePath("camera-lens-settings.png")));
            }
            frame = QImage(640, 480, QImage::Format_RGB32);
            frame.fill(Qt::black);
            page.setVideoFrame(frame, 2, 0);
            QVERIFY(!page.cameraView().lens.enabled);
            QVERIFY(!page.loadLensCalibration(writeLens(directory, lensCamera().lens), &error));
            check->setChecked(true);
            QVERIFY(!page.cameraView().lens.enabled);
        }
        AppSettings restored(settingsPath);
        QVERIFY(restored.liveLocalization().camera.lens.isValid());
        QVERIFY(!restored.liveLocalization().camera.lens.enabled);
    }
};

QTEST_MAIN(TestCameraLens)
#include "test_cameralens.moc"
