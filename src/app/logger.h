#pragma once

#include <QDate>
#include <QFile>
#include <QObject>
#include <QStringList>

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
    Logger(const QString &directory, qint64 maxBytes, int retainedFiles, QObject *parent = nullptr);
    ~Logger() override;

    void info(const QString &module, const QString &message);
    void warning(const QString &module, const QString &message);
    void error(const QString &module, const QString &message);
    QString currentFilePath() const { return m_file.fileName(); }
    QStringList logFiles() const;

signals:
    void messageLogged(LogLevel level, const QString &module, const QString &message, const QString &formatted);

private:
    bool openLogFile();
    void rotateIfNeeded(qint64 incomingBytes);
    void cleanupOldFiles();
    void log(LogLevel level, const QString &module, const QString &message);
    QFile m_file;
    QString m_directory;
    qint64 m_maxBytes = 10 * 1024 * 1024;
    int m_retainedFiles = 7;
    int m_sequence = 0;
    QDate m_openDate;
};

} // namespace MicArray
