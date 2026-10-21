from pathlib import Path

root = Path(__file__).resolve().parents[1]

def edit(name, before, after, count=1):
    p = root / name
    s = p.read_text(encoding="utf-8-sig")
    assert s.count(before) == count, (name, before[:70], s.count(before))
    p.write_text(s.replace(before, after), encoding="utf-8")

edit("CMakeLists.txt", "    src/analysis/pllocalizationcontroller.cpp", "    src/analysis/plspatialprojector.cpp\n    src/analysis/pllocalizationcontroller.cpp")
edit("CMakeLists.txt", "    src/media/audiolocationsideband.cpp", "    src/media/plspatialmap.cpp\n    src/media/audiolocationsideband.cpp")
edit("CMakeLists.txt", "    src/analysis/pllocalizationcontroller.h", "    src/analysis/plspatialprojector.h\n    src/analysis/pllocalizationcontroller.h")
edit("CMakeLists.txt", "    src/media/audiolocationsideband.h", "    src/media/plspatialmap.h\n    src/media/audiolocationsideband.h")
edit("src/media/audioprocessor.h", '#include "media/audiolocationsideband.h"', '#include "media/audiolocationsideband.h"\n#include "media/plspatialmap.h"')
edit("src/media/audioprocessor.h", "    void resetGain();", "    void resetGain();\n    void resetSpatialMaps(quint64 epoch);")
edit("src/media/audioprocessor.h", "    void plLocationDecoded(const PlLocationResult &location);", "    void plLocationDecoded(const PlLocationResult &location);\n    void plSpatialMapDecoded(const PlSpatialMap &map);")
edit("src/media/audioprocessor.h", "    ChannelGainProcessor m_gainProcessor;", "    ChannelGainProcessor m_gainProcessor;\n    PlSpatialMapAssembler m_spatialAssembler;\n    quint64 m_spatialEpoch = 0;")
edit("src/media/audioprocessor.cpp", "    emit rawFrameReady(sideband.pcmPayload, timestampSeconds, timestampMicros);", """    QStringList mapWarnings;
    auto maps = m_spatialAssembler.consume(payload, timestampSeconds, timestampMicros, &mapWarnings);
    for (PlSpatialMap &map : maps)
    {
        map.streamEpoch = m_spatialEpoch;
        emit plSpatialMapDecoded(map);
    }
    for (const QString &warning : mapWarnings)
    {
        emit audioWarning(warning);
    }
    emit rawFrameReady(sideband.pcmPayload, timestampSeconds, timestampMicros);""")
edit("src/media/audioprocessor.cpp", "void AudioProcessor::processArrayFrame(", """void AudioProcessor::resetSpatialMaps(quint64 epoch)
{
    m_spatialEpoch = epoch;
    m_spatialAssembler.reset();
}

void AudioProcessor::processArrayFrame(""")
edit("src/ui/mainwindow.h", "    bool m_controlDirty = false;", "    bool m_controlDirty = false;\n    quint64 m_plSpatialEpoch = 0;")
edit("src/ui/mainwindow.cpp", '    qRegisterMetaType<GainSnapshot>("MicArray::GainSnapshot");', '    qRegisterMetaType<GainSnapshot>("MicArray::GainSnapshot");\n    qRegisterMetaType<PlSpatialMap>("MicArray::PlSpatialMap");')
edit("src/ui/mainwindow.cpp", "    connect(m_audioProcessor, &AudioProcessor::rawFrameReady, this,", """    connect(m_audioProcessor, &AudioProcessor::plSpatialMapDecoded,
            this, [this](const PlSpatialMap &map)
    {
        if (m_client.state() == DeviceState::Connected && map.streamEpoch == m_plSpatialEpoch)
        {
            m_liveLocalizationPage->setPlSpatialMap(map);
        }
    }, Qt::QueuedConnection);
    connect(m_audioProcessor, &AudioProcessor::rawFrameReady, this,""")
edit("src/ui/mainwindow.cpp", "void MainWindow::updateConnectionState(DeviceState state)\n{", """void MainWindow::updateConnectionState(DeviceState state)
{
    const quint64 epoch = ++m_plSpatialEpoch;
    QMetaObject::invokeMethod(m_audioProcessor, [processor = m_audioProcessor, epoch]
    {
        processor->resetSpatialMaps(epoch);
    }, Qt::QueuedConnection);""")
edit("src/analysis/pllocalizationcontroller.h", "    bool stableCalibrationSample(", """    bool isCurrentFrame(quint16 frame) const
    {
        return m_hasPacket && !m_expired
            && quint16(m_lastPacket.locatorFrameCounter) == frame;
    }
    bool stableCalibrationSample(""")
edit("src/ui/livelocalizationpage.h", '#include "analysis/pllocalizationcontroller.h"', '#include "analysis/pllocalizationcontroller.h"\n#include "analysis/plspatialprojector.h"')
edit("src/ui/livelocalizationpage.h", "    void clearPlLocation();", "    void clearPlLocation();\n    void setPlSpatialMap(const PlSpatialMap &map);")
edit("src/ui/livelocalizationpage.h", "    bool plFusionSelected() const;", "    bool plFusionSelected() const;\n    bool plHeatmapSelected() const;")
edit("src/ui/livelocalizationpage.h", "    PlLocalizationController m_plController;", "    PlLocalizationController m_plController;\n    PlSpatialProjector m_plProjector;")
edit("src/ui/livelocalizationpage.cpp", "void LiveLocalizationPage::clearPlLocation()", """bool LiveLocalizationPage::plHeatmapSelected() const
{
    return m_settings.localizationSource == QStringLiteral("pl")
        && m_settings.plDisplayMode == QStringLiteral("plheatmap");
}

void LiveLocalizationPage::setPlSpatialMap(const PlSpatialMap &map)
{
    if (!m_active || !m_settings.enabled || !plHeatmapSelected() || m_calibrating
        || !m_plController.isCurrentFrame(map.locatorFrameCounter))
    {
        return;
    }
    const auto response = m_plProjector.project(map, m_settings.camera, m_gridGeneration);
    m_view->setSpatialResponse(response);
    if (response.normalizedValues.isEmpty())
    {
        m_heatmapExpiryTimer.stop();
        m_referenceHeatmapLabel->setText(QStringLiteral("PL 热力图：本帧无可信声源，已清除"));
    }
    else
    {
        m_heatmapExpiryTimer.start(1000);
        m_referenceHeatmapLabel->setText(
            QStringLiteral("PL 热力图：37×37 扫描，8 位响应；定位点来自同帧多峰精化；帧 %1")
                .arg(map.locatorFrameCounter));
    }
}

void LiveLocalizationPage::clearPlLocation()""")
edit("src/ui/livelocalizationpage.cpp", "m_view->setSpatialResponseActive(processingActive);", "m_view->setSpatialResponseActive(processingActive || (m_active && m_settings.enabled && plHeatmapSelected()));")
edit("src/ui/livelocalizationpage.cpp", "m_view->setSpatialResponseActive(hostProcessingActive());", "m_view->setSpatialResponseActive(hostProcessingActive() || (m_active && m_settings.enabled && plHeatmapSelected()));")
edit("src/ui/livelocalizationpage.cpp", "        const QString mode = host ? m_settings.displayMode : m_settings.plDisplayMode;", """        if (!host)
        {
            m_displayModeCombo->addItem(QStringLiteral("PL 热力图 + PL 多声源"), QStringLiteral("plheatmap"));
        }
        const QString mode = host ? m_settings.displayMode : m_settings.plDisplayMode;""")
edit("src/ui/livelocalizationpage.cpp", '    m_displayModeCombo->setToolTip(host\n', '    m_displayModeCombo->setToolTip(plHeatmapSelected()\n        ? QStringLiteral("热力图和多个定位点均由 PL 计算；需要配套新版 bit，PS 继续转发")\n        : host\n')
edit("src/ui/livelocalizationpage.cpp", "m_referenceHeatmapLabel->setVisible(plFusionSelected());", "m_referenceHeatmapLabel->setVisible(plFusionSelected() || plHeatmapSelected());")
edit("src/ui/livelocalizationpage.cpp", 'QStringLiteral("上位机热力图：结果过期，已清除")', 'plHeatmapSelected() ? QStringLiteral("PL 热力图：结果过期，已清除") : QStringLiteral("上位机热力图：结果过期，已清除")')
edit("src/ui/livelocalizationview.cpp", 'const bool fusion = m_displayMode == QStringLiteral("fusion");', 'const bool fusion = m_displayMode == QStringLiteral("fusion") || m_displayMode == QStringLiteral("plheatmap");')
edit("src/ui/livelocalizationview.cpp", 'const QString sources = QStringLiteral("热力图：上位机参考   定位点：PL");', 'const QString sources = m_displayMode == QStringLiteral("plheatmap")\n            ? QStringLiteral("热力图：PL 空间响应 37×37   定位点：PL 多峰精化")\n            : QStringLiteral("热力图：上位机参考   定位点：PL");')
edit("src/app/appsettings.h", 'QString plDisplayMode = QStringLiteral("marker");', 'QString plDisplayMode = QStringLiteral("plheatmap");')
edit("src/app/appsettings.cpp", '&& value.plDisplayMode != QStringLiteral("fusion"))', '&& value.plDisplayMode != QStringLiteral("fusion")\n        && value.plDisplayMode != QStringLiteral("plheatmap"))')
print("PL_HEATMAP_HOST_CONNECTED")
