#include "analysis/directionprojection.h"
#include "analysis/plspatialprojector.h"
#include "app/appsettings.h"
#include "media/audioframedecoder.h"
#include "media/audiolocationsideband.h"
#include "media/plspatialmap.h"
#include "ui/livelocalizationpage.h"
#include "ui/livelocalizationview.h"

#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLineF>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

using namespace MicArray;

namespace {
quint8 packetCrc(const QByteArray &packet)
{
    quint8 crc = 0;
    for (int i = 0; i < 40; ++i)
    {
        crc ^= static_cast<quint8>(packet.at(i));
        for (int b = 0; b < 8; ++b)
        {
            crc = crc & 128u ? (crc << 1) ^ 7u : crc << 1;
        }
    }
    return crc;
}

QByteArray chunk(quint16 sequence, quint16 frame, int offset, quint8 mask = 3)
{
    QByteArray packet(41, '\0');
    packet.replace(0, 4, "HMP1");
    packet[4] = 1;
    packet[5] = mask == 0 ? 0 : char(0xf8u | mask);
    qToLittleEndian(sequence, packet.data() + 6);
    qToLittleEndian(frame, packet.data() + 8);
    qToLittleEndian<quint16>(offset, packet.data() + 10);
    const int length = mask == 0 ? 0 : std::min(25, 1369 - offset);
    packet[12] = char(length);
    packet[13] = 37;
    packet[14] = 37;
    for (int i = 0; i < length; ++i) { packet[15 + i] = char((offset + i) & 255); }
    packet[40] = char(packetCrc(packet));
    QByteArray group(48 * 4, '\0');
    for (int i = 0; i < 41; ++i) { group[(i + 7) * 4 + 3] = packet.at(i); }
    return group;
}

QByteArray fullMap(quint16 sequence, quint16 frame)
{
    QByteArray payload;
    for (int offset = 0; offset < 1369; offset += 25) { payload += chunk(sequence, frame, offset); }
    return payload;
}

void feedLocations(LiveLocalizationPage &page, quint16 frame, bool reverse = false)
{
    constexpr qint16 Az[] = {-3500, 0, 3500};
    constexpr qint16 El[] = {-2000, 2000, -1000};
    for (int slot = 0; slot < 3; ++slot)
    {
        const int source = reverse ? 2 - slot : slot;
        PlLocationResult location;
        location.recordType = 3;
        location.version = 4;
        location.flags = 7;
        location.sourceId = slot;
        location.locationValid = true;
        location.sequence = frame;
        location.locatorFrameCounter = frame;
        location.azimuthCentiDegrees = Az[source];
        location.elevationCentiDegrees = El[source];
        location.confidenceQ15 = 30000;
        page.setPlLocation(location);
    }
}
} // namespace

class TestPlHeatmap : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qRegisterMetaType<QVector<SoundSourceDirection>>();
        const int font = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        const auto families = QFontDatabase::applicationFontFamilies(font);
        if (!families.isEmpty()) { qApp->setFont(QFont(families.first(), 9)); }
    }

    void decodeActualRtlTransportAndKeepRawPcm()
    {
        QFile file(QStringLiteral(MICARRAY_PL_FIXTURE));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray payload;
        for (const QByteArray &line : file.readAll().split('\n'))
        {
            if (line.trimmed().isEmpty()) { continue; }
            bool ok = false;
            const quint32 word = line.trimmed().toUInt(&ok, 16);
            QVERIFY(ok);
            char bytes[4];
            qToLittleEndian(word, bytes);
            payload.append(bytes, 4);
        }
        QCOMPARE(payload.size(), AudioFrameDecoder::FramePayloadBytes);
        const auto decoded = AudioLocationSidebandDecoder::decode(payload, 1, 0);
        QVERIFY(decoded.valid);
        for (int sample = 0; sample < 200; ++sample)
        {
            for (int channel = 0; channel < 48; ++channel)
            {
                const qint32 expected = qint32(0xff800000u | (quint32(sample) << 8) | channel);
                QCOMPARE(qFromLittleEndian<qint32>(
                    decoded.pcmPayload.constData() + (sample * 48 + channel) * 4), expected);
            }
        }
        PlSpatialMapAssembler assembler;
        QVERIFY(assembler.consume(payload.left(18 * 192), 1, 0).isEmpty());
        const auto maps = assembler.consume(payload.mid(18 * 192), 1, 1000);
        QCOMPARE(maps.size(), 2);
        QCOMPARE(maps.first().sourceMask, quint8(3));
        QCOMPARE(maps.first().values.size(), 1369);
        QCOMPARE(maps.first().reliability, 1.0);
        for (int i = 0; i < 1369; ++i) { QCOMPARE(maps.first().values.at(i), (i & 255) / 255.0f); }
        QCOMPARE(maps.last().sourceMask, quint8(0));
        QVERIFY(maps.last().values.isEmpty());
    }

    void incompleteCrcAndConflictingChunksCannotPublish()
    {
        PlSpatialMapAssembler assembler;
        auto damaged = fullMap(1, 1);
        damaged[22 * 4 + 3] ^= 1;
        QStringList warnings;
        QVERIFY(assembler.consume(damaged, 1, 0, &warnings).isEmpty());
        QVERIFY(!warnings.isEmpty());
        QCOMPARE(assembler.consume(chunk(1, 1, 0), 1, 100).size(), 1);
        QVERIFY(assembler.consume(chunk(1, 1, 0), 1, 200).isEmpty());
        assembler.reset();
        QVERIFY(assembler.consume(chunk(2, 2, 0), 2, 0).isEmpty());
        auto conflict = chunk(2, 2, 0);
        conflict[22 * 4 + 3] ^= 1;
        QByteArray packet;
        for (int i = 0; i < 41; ++i) { packet += conflict.at((i + 7) * 4 + 3); }
        conflict[47 * 4 + 3] = char(packetCrc(packet));
        QVERIFY(assembler.consume(conflict + fullMap(2, 2), 2, 10).isEmpty());
    }

    void outOfOrderWrapAndStaleFrames()
    {
        PlSpatialMapAssembler assembler;
        QByteArray shuffled;
        for (int offset = 1350; offset >= 0; offset -= 25) { shuffled += chunk(65535, 65535, offset); }
        QCOMPARE(assembler.consume(shuffled, 3, 0).size(), 1);
        QCOMPARE(assembler.consume(fullMap(0, 0), 3, 100).size(), 1);
        QVERIFY(assembler.consume(fullMap(65534, 65534), 3, 200).isEmpty());
        QCOMPARE(assembler.consume(chunk(1, 1, 0, 0), 3, 300).size(), 1);
    }

    void projectorUsesPhysicalAnglesAndCamera()
    {
        PlSpatialMap map;
        map.sourceMask = 1;
        map.reliability = .9;
        map.values.fill(0.0f, 1369);
        map.values[21 * 37 + 23] = 1.0f;
        PlSpatialProjector projector;
        CameraViewSettings camera;
        for (const double center : {0.0, 10.0})
        {
            camera.centerAzimuthDegrees = center;
            const auto response = projector.project(map, camera, 7);
            QCOMPARE(response.generation, quint64(7));
            const auto maximum = std::max_element(response.normalizedValues.cbegin(), response.normalizedValues.cend());
            const int index = int(maximum - response.normalizedValues.cbegin());
            const QPointF actual((index % response.columns + .5) / response.columns,
                                 (index / response.columns + .5) / response.rows);
            SoundSourceDirection direction;
            direction.azimuthDegrees = 25;
            direction.elevationDegrees = 15;
            QVERIFY(QLineF(actual, projectDirection(direction, camera).normalizedPoint).length() < .025);
        }
        map.sourceMask = 0;
        QVERIFY(projector.project(map, camera, 8).normalizedValues.isEmpty());
    }

    void plModeShowsMultipleTrackedSourcesAndRejectsOldMaps()
    {
        QTemporaryDir directory;
        AppSettings settings(directory.filePath("settings.ini"));
        auto config = settings.liveLocalization();
        config.localizationSource = QStringLiteral("pl");
        config.plDisplayMode = QStringLiteral("plheatmap");
        settings.saveLiveLocalization(config);
        LiveLocalizationPage page(&settings);
        page.resize(1300, 760);
        QImage video(1280, 720, QImage::Format_RGB32);
        video.fill(QColor(35, 40, 48));
        {
            QPainter painter(&video);
            painter.setPen(Qt::white);
            painter.drawText(25, 35, QStringLiteral("PL 三声源合成 PCM 仿真演示"));
        }
        page.setVideoFrame(video, 1, 0);
        page.setActive(true);
        QSignalSpy directions(&page, &LiveLocalizationPage::plDirectionsChanged);
        for (int frame = 1; frame <= 3; ++frame) { feedLocations(page, frame); }
        const auto first = qvariant_cast<QVector<SoundSourceDirection>>(directions.last().first());
        QCOMPARE(first.size(), 3);
        feedLocations(page, 4, true);
        const auto second = qvariant_cast<QVector<SoundSourceDirection>>(directions.last().first());
        QCOMPARE(second.size(), 3);
        for (const auto &a : first)
        {
            const auto same = std::find_if(second.cbegin(), second.cend(), [&a](const auto &b)
            {
                return std::abs(a.azimuthDegrees - b.azimuthDegrees) < 1.0;
            });
            QVERIFY(same != second.cend());
            QCOMPARE(same->sourceId, a.sourceId);
        }
        PlSpatialMap map;
        map.sourceMask = 7;
        map.reliability = .9;
        map.values.fill(.3f, 1369);
        map.locatorFrameCounter = 4;
        page.setPlSpatialMap(map);
        auto *label = page.findChild<QLabel *>(QStringLiteral("referenceHeatmapStatusLabel"));
        QVERIFY(label->text().contains(QStringLiteral("37×37")));
        map.locatorFrameCounter = 3;
        page.setPlSpatialMap(map);
        QVERIFY(label->text().contains(QStringLiteral("帧 4")));
        if (!qEnvironmentVariable("MICARRAY_PL_SCREENSHOT").isEmpty())
        {
            QFile data(QStringLiteral(MICARRAY_PL_MAP_FIXTURE));
            QVERIFY(data.open(QIODevice::ReadOnly));
            map.values.clear();
            for (const auto &line : data.readAll().split('\n'))
            {
                if (!line.trimmed().isEmpty()) { map.values.append(line.trimmed().toUInt(nullptr, 16) / 255.0f); }
            }
            QCOMPARE(map.values.size(), 1369);
            map.locatorFrameCounter = 4;
            page.setPlSpatialMap(map);
            page.findChild<LiveLocalizationView *>()->setStatusText(
                QStringLiteral("PL 三声源合成 PCM 仿真演示（非实机测试）"));
            page.show();
            QTest::qWait(50);
            QVERIFY(page.grab().save(qEnvironmentVariable("MICARRAY_PL_SCREENSHOT")));
        }
        QTest::qWait(1100);
        QVERIFY(label->text().contains(QStringLiteral("过期")));
        map.locatorFrameCounter = 4;
        page.setPlSpatialMap(map);
        QVERIFY(label->text().contains(QStringLiteral("过期")));
        page.clearPlLocation();
        map.locatorFrameCounter = 4;
        page.setPlSpatialMap(map);
        QVERIFY(!label->text().contains(QStringLiteral("帧 4")));
        page.setActive(false);
    }
};

QTEST_MAIN(TestPlHeatmap)
#include "test_plheatmap.moc"
