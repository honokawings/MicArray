#pragma once

#include <QFile>
#include <QObject>

namespace MicArray {

enum class LogLevel {
    Info,
    Warning,
    Error,
};

QString logLevelName(LogLevel level);

class Logger : public QObject
{
    Q_OBJECT

public:
    explicit Logger(QObject *parent = nullptr);
    ~Logger() override;

    void info(const QString &module, const QString &message);
    void warning(const QString &module, const QString &message);
    void error(const QString &module, const QString &message);

signals:
    void messageLogged(LogLevel level, const QString &module, const QString &message, const QString &formatted);

private:
    void log(LogLevel level, const QString &module, const QString &message);
    QFile m_file;
};

} // namespace MicArray
