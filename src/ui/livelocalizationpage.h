#pragma once

#include "analysis/livelocalizationworker.h"
#include "app/appsettings.h"

#include <QImage>
#include <QThread>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace MicArray {

class LiveLocalizationView;

class LiveLocalizationPage : public QWidget
{
    Q_OBJECT

public:
    explicit LiveLocalizationPage(AppSettings *settings, QWidget *parent = nullptr);
    ~LiveLocalizationPage() override;

    void setActive(bool active);
    void setVideoFrame(const QImage &image,
                       quint32 timestampSeconds,
                       quint32 timestampMicros);
    void submitAdjustedAudioFrame(const QByteArray &payload,
                                  quint32 timestampSeconds,
                                  quint32 timestampMicros);
    bool saveCalibrationProfile(const QString &filePath,
                                QString *errorMessage = nullptr) const;
    bool loadCalibrationProfile(const QString &filePath,
                                QString *errorMessage = nullptr);

signals:
    void configureWorkerRequested(const LiveLocalizationSettings &settings);
    void workerActiveRequested(bool active);
    void spatialGridRequested(const SpatialResponseGrid &grid, quint64 generation);
    void audioFrameRequested(const QByteArray &payload,
                             quint32 timestampSeconds,
                             quint32 timestampMicros);
    void calibrationSampleRequested(quint64 requestId);

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    static constexpr int MaxPendingAudioFrames = 32;

    void buildUi();
    void connectUi();
    void restoreSettings();
    void persistSettings();
    void applyCameraToUi(const CameraViewSettings &camera);
    void updateCameraFromUi(bool persist);
    void startNewProcessingEpoch();
    void rebuildSpatialGrid();
    void startCalibration();
    void cancelCalibration();
    void undoLastCalibrationPoint();
    void clearCalibration();
    void acceptCalibrationDirection(quint64 requestId,
                                    bool valid,
                                    VideoCalibrationSample sample,
                                    const QString &message);
    void finishCalibration();
    void refreshCalibrationOverlay();
    void refreshCalibrationDetails();
    void updateCalibrationTableHeight();

    AppSettings *m_settingsStore = nullptr;
    LiveLocalizationSettings m_settings;
    CameraViewSettings m_savedCamera;
    QThread m_workerThread;
    LiveLocalizationWorker *m_worker = nullptr;
    LiveLocalizationView *m_view = nullptr;
    QImage m_videoFrame;
    QVector<VideoCalibrationSample> m_workingSamples;
    QPointF m_pendingCalibrationPoint;
    quint64 m_calibrationRequestId = 0;
    quint64 m_gridGeneration = 0;
    int m_pendingAudioFrames = 0;
    bool m_active = false;
    bool m_calibrating = false;

    QCheckBox *m_enabledCheck = nullptr;
    QComboBox *m_locatorCombo = nullptr;
    QComboBox *m_displayModeCombo = nullptr;
    QComboBox *m_spatialResolutionModeCombo = nullptr;
    QCheckBox *m_neuralPeakEnhancementCheck = nullptr;
    QDoubleSpinBox
        *m_neuralPeakEnhancementStrengthSpin = nullptr;
    QDoubleSpinBox *m_heatmapOpacitySpin = nullptr;
    QDoubleSpinBox *m_heatmapFocusSpin = nullptr;
    QSpinBox *m_heatmapGridColumnsSpin = nullptr;
    QSpinBox *m_heatmapGridRowsSpin = nullptr;
    QSpinBox *m_updateIntervalSpin = nullptr;
    QDoubleSpinBox *m_confidenceSpin = nullptr;
    QDoubleSpinBox *m_centerAzimuthSpin = nullptr;
    QDoubleSpinBox *m_centerElevationSpin = nullptr;
    QDoubleSpinBox *m_horizontalFovSpin = nullptr;
    QDoubleSpinBox *m_verticalFovSpin = nullptr;
    QDoubleSpinBox *m_rollSpin = nullptr;
    QPushButton *m_startCalibrationButton = nullptr;
    QPushButton *m_cancelCalibrationButton = nullptr;
    QPushButton *m_undoCalibrationButton = nullptr;
    QPushButton *m_clearCalibrationButton = nullptr;
    QPushButton *m_exportCalibrationButton = nullptr;
    QPushButton *m_importCalibrationButton = nullptr;
    QCheckBox *m_showCalibrationOverlayCheck = nullptr;
    QTableWidget *m_calibrationSamplesTable = nullptr;
    QPushButton *m_restoreSavedButton = nullptr;
    QPushButton *m_restoreDefaultsButton = nullptr;
    QLabel *m_progressLabel = nullptr;
    QLabel *m_rmsLabel = nullptr;
    QLabel *m_resultLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
};

} // namespace MicArray
