#include "app/logger.h"

#include <QDate>
#include <QDir>
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
    : QObject(parent)
{
    const QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(basePath);
    m_file.setFileName(basePath + QLatin1String("/micarray-") + QDate::currentDate().toString(QStringLiteral("yyyyMMdd")) + QLatin1String(".log"));
    if (!m_file.open(QIODevice::Append | QIODevice::Text)) {
        m_file.setFileName(QString());
    }
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
    if (m_file.isOpen()) {
        QTextStream stream(&m_file);
        stream << formatted << '\n';
        m_file.flush();
    }
    emit messageLogged(level, module, message, formatted);
}

} // namespace MicArray
