#include "app/logger.h"

#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QStandardPaths>
#include <QTextStream>
#include <QTime>

namespace MicArray {

QString logLevelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Info:
        return QStringLiteral("INFO");
    case LogLevel::Warning:
        return QStringLiteral("WARN");
    case LogLevel::Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("INFO");
}

Logger::Logger(QObject *parent)
    : Logger(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
             10 * 1024 * 1024,
             7,
             parent)
{
}

Logger::Logger(const QString &directory, qint64 maxBytes, int retainedFiles, QObject *parent)
    : QObject(parent)
    , m_directory(directory)
    , m_maxBytes(std::max<qint64>(1, maxBytes))
    , m_retainedFiles(std::max(1, retainedFiles))
{
    QDir().mkpath(m_directory);
    openLogFile();
}

Logger::~Logger()
{
    if (m_file.isOpen()) {
        m_file.close();
    }
}

void Logger::info(const QString &module, const QString &message)
{
    log(LogLevel::Info, module, message);
}

void Logger::warning(const QString &module, const QString &message)
{
    log(LogLevel::Warning, module, message);
}

void Logger::error(const QString &module, const QString &message)
{
    log(LogLevel::Error, module, message);
}

void Logger::log(LogLevel level, const QString &module, const QString &message)
{
    const QString formatted = QStringLiteral("%1 [%2] %3: %4")
                                  .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                       logLevelName(level),
                                       module,
                                       message);
    const QByteArray encoded = (formatted + QLatin1Char('\n')).toUtf8();
    rotateIfNeeded(encoded.size());
    if (m_file.isOpen()) {
        m_file.write(encoded);
        m_file.flush();
    }
    emit messageLogged(level, module, message, formatted);
}

bool Logger::openLogFile()
{
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_openDate = QDate::currentDate();
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString fileName = QStringLiteral("micarray-%1-%2.log").arg(stamp).arg(m_sequence++, 3, 10, QLatin1Char('0'));
    m_file.setFileName(QDir(m_directory).filePath(fileName));
    if (!m_file.open(QIODevice::Append | QIODevice::Text)) {
        m_file.setFileName(QString());
        return false;
    }
    cleanupOldFiles();
    return true;
}

void Logger::rotateIfNeeded(qint64 incomingBytes)
{
    if (!m_file.isOpen()) {
        openLogFile();
        return;
    }
    if (m_openDate != QDate::currentDate() || (m_file.size() > 0 && m_file.size() + incomingBytes > m_maxBytes)) {
        openLogFile();
    }
}

QStringList Logger::logFiles() const
{
    QStringList paths;
    const QFileInfoList entries = QDir(m_directory).entryInfoList({QStringLiteral("micarray-*.log")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (const QFileInfo &entry : entries) {
        paths.append(entry.absoluteFilePath());
    }
    return paths;
}

void Logger::cleanupOldFiles()
{
    const QFileInfoList entries = QDir(m_directory).entryInfoList({QStringLiteral("micarray-*.log")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (int index = m_retainedFiles; index < entries.size(); ++index) {
        QFile::remove(entries.at(index).absoluteFilePath());
    }
}

} // namespace MicArray
