#include "diagnostics/diagnosticexporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace MicArray {
namespace {

bool writeFile(const QString &path, const QByteArray &contents, QString *errorMessage)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    return true;
}

QString createBundleDirectory(const QString &baseDirectory)
{
    const QString stem = QStringLiteral("MicArray-diagnostics-%1")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QDir base(baseDirectory);
    for (int suffix = 0; suffix < 1000; ++suffix) {
        const QString name = suffix == 0 ? stem : QStringLiteral("%1-%2").arg(stem).arg(suffix, 3, 10, QLatin1Char('0'));
        const QString path = base.filePath(name);
        if (!QFileInfo::exists(path) && QDir().mkpath(path)) {
            return QDir(path).absolutePath();
        }
    }
    return {};
}

QJsonObject sessionObject(const SessionMetrics &metrics)
{
    QJsonObject object;
    object.insert(QStringLiteral("startedAt"), metrics.startedAt.toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("endedAt"), metrics.endedAt.toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("durationSeconds"), metrics.startedAt.isValid() && metrics.endedAt.isValid()
                      ? metrics.startedAt.msecsTo(metrics.endedAt) / 1000.0
                      : 0.0);
    object.insert(QStringLiteral("totalBytes"), static_cast<qint64>(metrics.totalBytes));
    object.insert(QStringLiteral("totalFrames"), static_cast<qint64>(metrics.totalFrames));
    object.insert(QStringLiteral("parseErrors"), static_cast<qint64>(metrics.parseErrors));
    object.insert(QStringLiteral("reconnectCount"), static_cast<qint64>(metrics.reconnectCount));
    object.insert(QStringLiteral("audioDroppedFrames"), static_cast<qint64>(metrics.audioDroppedFrames));
    object.insert(QStringLiteral("videoDroppedFrames"), static_cast<qint64>(metrics.videoDroppedFrames));
    object.insert(QStringLiteral("averageReceiveMbps"), metrics.averageReceiveMbps);
    object.insert(QStringLiteral("peakReceiveMbps"), metrics.peakReceiveMbps);
    return object;
}

} // namespace

QString DiagnosticExporter::exportBundle(const QString &baseDirectory,
                                          const DiagnosticBundle &bundle,
                                          const QStringList &logFiles,
                                          QString *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    const QString directory = createBundleDirectory(baseDirectory);
    if (directory.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not create diagnostic directory");
        }
        return {};
    }

    QDir output(directory);
    if (!writeFile(output.filePath(QStringLiteral("session.json")),
                   QJsonDocument(sessionObject(bundle.session)).toJson(QJsonDocument::Indented),
                   errorMessage)
        || !writeFile(output.filePath(QStringLiteral("settings.json")),
                      QJsonDocument(QJsonObject::fromVariantMap(bundle.settings)).toJson(QJsonDocument::Indented),
                      errorMessage)
        || !writeFile(output.filePath(QStringLiteral("events.txt")),
                      (bundle.events.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8(),
                      errorMessage)) {
        return {};
    }

    const QString logsDirectory = output.filePath(QStringLiteral("logs"));
    QDir().mkpath(logsDirectory);
    for (const QString &source : logFiles) {
        const QFileInfo info(source);
        if (!info.exists()) {
            continue;
        }
        QString target = QDir(logsDirectory).filePath(info.fileName());
        for (int suffix = 1; QFileInfo::exists(target); ++suffix) {
            target = QDir(logsDirectory).filePath(QStringLiteral("%1-%2.%3")
                                                       .arg(info.completeBaseName())
                                                       .arg(suffix)
                                                       .arg(info.suffix()));
        }
        if (!QFile::copy(source, target) && errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not copy log file %1").arg(source);
            return {};
        }
    }
    return directory;
}

} // namespace MicArray
