#include "fusioncontext.h"

#include <QJsonArray>
#include <QTest>

#include <array>
#include <cmath>
#include <limits>

using namespace MicArray::Array128;

namespace {
constexpr qint64 Ms = 1000000;

ProcessingSettings matchingSettings()
{
    ProcessingSettings settings;
    settings.camera = {0, 0, 90, 60, 0};
    settings.gridColumns = 72;
    settings.gridRows = 45;
    settings.soundSpeed = 343;
    settings.maxSources = 4;
    settings.minimumSourceSeparationDegrees = 4;
    for (int ch = 0; ch < 128; ++ch) {
        const double radius = .015 * std::sqrt(ch + 1.0);
        const double angle = ch * 2.399963229728653;
        settings.geometry.microphones.append({radius * std::cos(angle), radius * std::sin(angle), 0});
    }
    return settings;
}

QJsonObject matchingMetadata(const ProcessingSettings &settings)
{
    QJsonArray coordinates;
    for (const auto &point : settings.geometry.microphones)
        coordinates.append(QJsonArray {point.xMeters, point.yMeters, point.zMeters});
    return {{"geometry_coordinates_m", coordinates}, {"training_srp_grid", QJsonArray {72, 45}},
        {"training_srp_settings", QJsonObject {{"max_sources", 4}, {"minimum_source_separation_deg", 4},
            {"sound_speed_m_s", 343}, {"window_samples", 2048}}}};
}

QImage solid(const QColor &color)
{
    QImage image(17, 11, QImage::Format_RGB888);
    image.fill(color);
    return image;
}
}

class FusionContextTest : public QObject
{
    Q_OBJECT
private slots:
    void matchingTrainingDomainIsAccepted()
    {
        const auto settings = matchingSettings();
        QVERIFY(fusionDomainError(matchingMetadata(settings), settings).isEmpty());
    }

    void channelOrderAndCoordinatesMustMatch()
    {
        auto settings = matchingSettings();
        const auto metadata = matchingMetadata(settings);
        std::swap(settings.geometry.microphones[0], settings.geometry.microphones[127]);
        QVERIFY(!fusionDomainError(metadata, settings).isEmpty());
        settings = matchingSettings();
        settings.geometry.microphones[17].xMeters += .00001;
        QVERIFY(!fusionDomainError(metadata, settings).isEmpty());
        settings = matchingSettings();
        settings.geometry.microphones.removeLast();
        QVERIFY(!fusionDomainError(metadata, settings).isEmpty());
        QVERIFY(!fusionDomainError({}, matchingSettings()).isEmpty());
    }

    void malformedOrNonfiniteGeometryFailsClosed()
    {
        const auto baseline = matchingSettings();
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
            auto settings = baseline;
            settings.geometry.microphones[12].yMeters = invalid;
            QVERIFY(!fusionDomainError(matchingMetadata(baseline), settings).isEmpty());
        }
        for (const QJsonValue invalid : {QJsonValue(QJsonValue::Null), QJsonValue("0.012"), QJsonValue(true)}) {
            auto metadata = matchingMetadata(baseline);
            auto points = metadata["geometry_coordinates_m"].toArray();
            auto point = points[0].toArray(); point[0] = invalid; points[0] = point;
            metadata["geometry_coordinates_m"] = points;
            QVERIFY(!fusionDomainError(metadata, baseline).isEmpty());
        }
    }

    void gridAndSoundSpeedAreTrainingContracts()
    {
        auto settings = matchingSettings();
        const auto metadata = matchingMetadata(settings);
        settings.gridColumns = 160;
        QVERIFY(!fusionDomainError(metadata, settings).isEmpty());
        settings = matchingSettings(); settings.gridRows = 100;
        QVERIFY(!fusionDomainError(metadata, settings).isEmpty());
        auto changedMetadata = metadata;
        changedMetadata["training_srp_grid"] = QJsonArray {160, 100};
        settings.gridColumns = 160;
        QVERIFY2(!fusionDomainError(changedMetadata, settings).isEmpty(), "Matching unsupported dimensions must not bypass the fixed training contract");
        changedMetadata.remove("training_srp_grid");
        QVERIFY(!fusionDomainError(changedMetadata, matchingSettings()).isEmpty());
        for (double invalid : {340.0, 346.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
            settings = matchingSettings(); settings.soundSpeed = invalid;
            QVERIFY2(!fusionDomainError(metadata, settings).isEmpty(), "Nonfinite/out-of-domain sound speed must fail closed");
        }
    }

    void eachCameraParameterMustBeFiniteAndWithinTrainingBounds()
    {
        const std::array<double MicArray::CameraViewSettings::*, 5> fields {{
            &MicArray::CameraViewSettings::centerAzimuthDegrees,
            &MicArray::CameraViewSettings::centerElevationDegrees,
            &MicArray::CameraViewSettings::horizontalFovDegrees,
            &MicArray::CameraViewSettings::verticalFovDegrees,
            &MicArray::CameraViewSettings::rollDegrees}};
        const std::array<double, 5> lower {{-8, -5, 75, 48, -5}};
        const std::array<double, 5> upper {{8, 5, 100, 67, 5}};
        const auto metadata = matchingMetadata(matchingSettings());
        for (int field = 0; field < 5; ++field) {
            for (double valid : {lower[field], upper[field]}) {
                auto settings = matchingSettings(); settings.camera.*fields[field] = valid;
                QVERIFY(fusionDomainError(metadata, settings).isEmpty());
            }
            for (double invalid : {lower[field] - .01, upper[field] + .01,
                                   std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity()}) {
                auto settings = matchingSettings(); settings.camera.*fields[field] = invalid;
                QVERIFY2(!fusionDomainError(metadata, settings).isEmpty(), "Camera parameter must not bypass training domain");
            }
        }
    }

    void peakExtractionParametersMustMatchTrainingHeat()
    {
        const auto baseline = matchingSettings();
        const auto metadata = matchingMetadata(baseline);
        for (int limit : {1, 3, 5, 16}) {
            auto settings = baseline; settings.maxSources = limit;
            QVERIFY2(!fusionDomainError(metadata, settings).isEmpty(), "Peak budget changes the preserved fine-peak SRP raster");
        }
        for (double distance : {2.0, 8.0, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()}) {
            auto settings = baseline; settings.minimumSourceSeparationDegrees = distance;
            QVERIFY2(!fusionDomainError(metadata, settings).isEmpty(), "Minimum source separation must match the trained SRP producer");
        }
        auto missing = metadata; missing.remove("training_srp_settings");
        QVERIFY(!fusionDomainError(missing, baseline).isEmpty());
        for (const auto &key : {QString("max_sources"), QString("minimum_source_separation_deg"),
                               QString("sound_speed_m_s"), QString("window_samples")}) {
            auto changed = metadata;
            auto contract = changed["training_srp_settings"].toObject();
            contract[key] = contract[key].toDouble() + 1;
            changed["training_srp_settings"] = contract;
            QVERIFY2(!fusionDomainError(changed, baseline).isEmpty(), qPrintable(key));
        }
    }

    void moreMaskedChannelsThanTrainingCoverageIsRejected()
    {
        auto settings = matchingSettings();
        const auto metadata = matchingMetadata(settings);
        for (int ch = 0; ch < 16; ++ch) settings.disabledChannels[ch] = true;
        QVERIFY(fusionDomainError(metadata, settings).isEmpty());
        settings.disabledChannels[16] = true;
        QVERIFY2(!fusionDomainError(metadata, settings).isEmpty(), "More than 16 masked channels lies outside the trained PSF domain");
    }

    void emptyAndInvalidTimesAreRejected()
    {
        FusionVideoBuffer buffer;
        QVERIFY(!buffer.nearest(10 * Ms, 20 * Ms).error.isEmpty());
        buffer.append({}, 0); buffer.append(solid(Qt::red), -1);
        QVERIFY(!buffer.nearest(0, 0).error.isEmpty());
        buffer.append(solid(Qt::red), 0);
        QVERIFY(!buffer.nearest(-1, Ms).error.isEmpty());
        QVERIFY(!buffer.nearest(2 * Ms, Ms).error.isEmpty());
        QVERIFY(!buffer.nearest(0, 0, -1, 500).error.isEmpty());
        QVERIFY(!buffer.nearest(0, 0, 100, 0).error.isEmpty());
        QVERIFY(!buffer.nearest(0, 0, std::numeric_limits<double>::quiet_NaN(), 500).error.isEmpty());
        QVERIFY(!buffer.nearest(0, 0, 100, std::numeric_limits<double>::infinity()).error.isEmpty());
    }

    void nearestFrameUsesSingleClockAndRejectsFutureFrames()
    {
        FusionVideoBuffer buffer;
        buffer.append(solid(Qt::red), 100 * Ms);
        buffer.append(solid(Qt::green), 130 * Ms);
        buffer.append(solid(Qt::blue), 200 * Ms);
        const auto result = buffer.nearest(125 * Ms, 150 * Ms);
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.image.pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(result.skewMs, 5.0);
        QCOMPARE(result.ageMs, 25.0); // audio is older than the selected video
        const auto historical = buffer.nearest(145 * Ms, 150 * Ms);
        QVERIFY(historical.error.isEmpty());
        QCOMPARE(historical.skewMs, -15.0);
        QCOMPARE(historical.ageMs, 20.0); // video is older in this pairing
        FusionVideoBuffer futureOnly; futureOnly.append(solid(Qt::blue), 200 * Ms);
        QVERIFY(!futureOnly.nearest(150 * Ms, 150 * Ms).error.isEmpty());
    }

    void skewAndOldestInputAgeHaveInclusiveBoundaries()
    {
        FusionVideoBuffer buffer;
        buffer.append(solid(Qt::red), 100 * Ms);
        QVERIFY(buffer.nearest(200 * Ms, 600 * Ms, 100, 500).error.isEmpty());
        QVERIFY(!buffer.nearest(200 * Ms + 1, 600 * Ms, 100, 500).error.isEmpty());
        QVERIFY(!buffer.nearest(200 * Ms, 600 * Ms + 1, 100, 500).error.isEmpty());
        FusionVideoBuffer recentVideo; recentVideo.append(solid(Qt::blue), 500 * Ms);
        const auto oldAudio = recentVideo.nearest(100 * Ms, 600 * Ms, 500, 450);
        QVERIFY(!oldAudio.error.isEmpty());
        QCOMPARE(oldAudio.ageMs, 500.0); // do not report only the 100-ms video age
    }

    void clearOrderingAndBoundedHistory()
    {
        FusionVideoBuffer buffer;
        buffer.append(solid(Qt::red), 100 * Ms);
        buffer.append(solid(Qt::blue), 100 * Ms); // same timestamp must not replace the snapshot
        buffer.append(solid(Qt::green), 90 * Ms);
        QCOMPARE(buffer.nearest(100 * Ms, 100 * Ms).image.pixelColor(0, 0), QColor(Qt::red));
        buffer.clear();
        QVERIFY(!buffer.nearest(100 * Ms, 100 * Ms).error.isEmpty());
        for (int i = 0; i <= 32; ++i) buffer.append(solid(QColor(i, 0, 0)), i * Ms);
        QVERIFY(!buffer.nearest(0, 32 * Ms, 0, 500).error.isEmpty()); // oldest of 33 is evicted
        QVERIFY(buffer.nearest(32 * Ms, 32 * Ms, 0, 500).error.isEmpty());
        buffer.append(solid(Qt::cyan), 700 * Ms);
        QVERIFY(!buffer.nearest(32 * Ms, 700 * Ms, 0, 1000).error.isEmpty()); // 600-ms retention limit
    }

    void bufferingPreservesPixelsAndDoesNotModifySource()
    {
        QImage source(53, 27, QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x)
            source.setPixel(x, y, qRgb(x * 4 % 256, y * 7 % 256, (x + y) * 3 % 256));
        const QImage before = source.copy();
        FusionVideoBuffer buffer; buffer.append(source, 10 * Ms);
        QCOMPARE(source, before);
        const auto result = buffer.nearest(10 * Ms, 20 * Ms);
        QVERIFY(result.error.isEmpty());
        QCOMPARE(result.image.size(), source.size());
        QCOMPARE(result.image, before.convertToFormat(QImage::Format_RGB888));
        source.fill(Qt::black);
        QCOMPARE(buffer.nearest(10 * Ms, 20 * Ms).image, result.image);
        // The returned QImage is also an implicit shared snapshot: changing a
        // caller's copy must not mutate the retained frame.
        QImage caller = result.image; caller.fill(Qt::white);
        QCOMPARE(buffer.nearest(10 * Ms, 20 * Ms).image, result.image);
    }

    void displayHistoryCoversFiveHundredMillisecondInferenceLifetime()
    {
        FusionDisplayBuffer buffer;
        LocalizationResult acoustic; acoustic.generation = 7; acoustic.run = 3;
        for (quint64 frame = 1; frame <= 6; ++frame) {
            acoustic.lastFrame = frame;
            buffer.store(solid(QColor(int(frame), 10, 20)), acoustic);
        }
        const auto first = buffer.find(7, 3, 1);
        QVERIFY2(first.has_value(), "At 10 Hz frame 1 must survive submission of frames 2..6 within its 500 ms lifetime");
        QCOMPARE(first->video.pixelColor(0, 0), QColor(1, 10, 20));
        for (quint64 frame = 7; frame <= 9; ++frame) {
            acoustic.lastFrame = frame;
            buffer.store(solid(QColor(int(frame), 10, 20)), acoustic);
        }
        QVERIFY(!buffer.find(7, 3, 1).has_value());
        QVERIFY(buffer.find(7, 3, 2).has_value());
        QVERIFY(buffer.find(7, 3, 9).has_value());
    }

    void displayHistoryMatchesAllIdentityFieldsAndClears()
    {
        FusionDisplayBuffer buffer;
        LocalizationResult acoustic; acoustic.generation = 7; acoustic.run = 3; acoustic.lastFrame = 101;
        buffer.store(solid(Qt::red), acoustic);
        acoustic.generation = 8; buffer.store(solid(Qt::blue), acoustic);
        acoustic.run = 4; buffer.store(solid(Qt::green), acoustic);
        const auto original = buffer.find(7, 3, 101);
        const auto configured = buffer.find(8, 3, 101);
        const auto restarted = buffer.find(8, 4, 101);
        QVERIFY(original.has_value()); QVERIFY(configured.has_value()); QVERIFY(restarted.has_value());
        QCOMPARE(original->video.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(configured->video.pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(restarted->video.pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(!buffer.find(7, 4, 101).has_value());
        QVERIFY(!buffer.find(8, 4, 100).has_value());
        QVERIFY(!buffer.find(9, 4, 101).has_value());
        buffer.clear();
        QVERIFY(!buffer.find(7, 3, 101).has_value());
        QVERIFY(!buffer.find(8, 3, 101).has_value());
        QVERIFY(!buffer.find(8, 4, 101).has_value());
    }

    void displayPreviewIsBoundedWithoutChangingInferencePixelsOrRawAcoustics()
    {
        QImage source(1920, 1080, QImage::Format_ARGB32);
        source.fill(QColor(35, 70, 105));
        source.setPixelColor(0, 0, Qt::red);
        source.setPixelColor(1919, 1079, Qt::blue);
        const auto originalPixels = source.copy();
        LocalizationResult acoustic;
        acoustic.generation = 7; acoustic.run = 3; acoustic.lastFrame = 101;
        acoustic.observationTimeNs = 123456789;
        acoustic.columns = 3; acoustic.rows = 2;
        acoustic.heat = {.0f, .25f, .5f, .75f, 1.0f, .125f};
        acoustic.sources = {{{12.25, -6.5}, .625, 1.0, true}, {{-24.5, 8.25}, .5, .75, false}};
        const auto originalAcoustic = acoustic;
        FusionDisplayBuffer buffer; buffer.store(source, acoustic);
        QCOMPARE(source.size(), QSize(1920, 1080));
        QCOMPARE(source, originalPixels);
        // Caller mutations after store must not overwrite the retained source
        // response or candidates through implicitly shared Qt containers.
        acoustic.heat[0] = -.5f;
        acoustic.sources[0].direction.azimuthDegrees = 80;
        const auto context = buffer.find(7, 3, 101);
        QVERIFY(context.has_value());
        QVERIFY(context->video.width() <= 1280 && context->video.height() <= 720);
        QCOMPARE(context->video.size(), QSize(1280, 720));
        QCOMPARE(context->localization.columns, originalAcoustic.columns);
        QCOMPARE(context->localization.rows, originalAcoustic.rows);
        QCOMPARE(context->localization.observationTimeNs, originalAcoustic.observationTimeNs);
        QCOMPARE(context->localization.heat, originalAcoustic.heat);
        QCOMPARE(context->localization.sources.size(), originalAcoustic.sources.size());
        for (int i = 0; i < originalAcoustic.sources.size(); ++i) {
            const auto &actual = context->localization.sources[i], &expected = originalAcoustic.sources[i];
            QCOMPARE(actual.direction.azimuthDegrees, expected.direction.azimuthDegrees);
            QCOMPARE(actual.direction.elevationDegrees, expected.direction.elevationDegrees);
            QCOMPARE(actual.confidence, expected.confidence);
            QCOMPARE(actual.relativePower, expected.relativePower);
            QCOMPARE(actual.trusted, expected.trusted);
        }
    }
};

QTEST_GUILESS_MAIN(FusionContextTest)
#include "test_fusioncontext.moc"
