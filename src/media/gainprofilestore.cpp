#include "media/gainprofilestore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

#include <cmath>
#include <limits>

namespace MicArray {

namespace {

constexpr int ProfileSchemaVersion = 1;

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

bool readInteger(const QJsonValue &value, int *result)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number)
        || std::trunc(number) != number
        || number < std::numeric_limits<int>::min()
        || number > std::numeric_limits<int>::max()) {
        return false;
    }
    *result = static_cast<int>(number);
    return true;
}

bool readNumber(const QJsonValue &value, double *result)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        return false;
    }
    *result = value.toDouble();
    return true;
}

bool validateKeys(const QJsonObject &object,
                  const QStringList &expectedKeys,
                  const QString &objectName,
                  QString *error)
{
    for (const QString &key : object.keys()) {
        if (!expectedKeys.contains(key)) {
            setError(error, QStringLiteral("Gain profile %1 contains unexpected key %2")
                                .arg(objectName, key));
            return false;
        }
    }
    for (const QString &key : expectedKeys) {
        if (!object.contains(key)) {
            setError(error, QStringLiteral("Gain profile %1 is missing required key %2")
                                .arg(objectName, key));
            return false;
        }
    }
    return true;
}

QJsonObject toJsonObject(const GainConfiguration &configuration)
{
    QJsonArray shifts;
    QJsonArray automatic;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        shifts.append(configuration.shifts.at(channel));
        automatic.append(configuration.automatic.at(channel));
    }

    return {
        {QStringLiteral("schemaVersion"), ProfileSchemaVersion},
        {QStringLiteral("channelCount"), GainConfiguration::ChannelCount},
        {QStringLiteral("shifts"), shifts},
        {QStringLiteral("automatic"), automatic},
        {QStringLiteral("agc"), QJsonObject {
            {QStringLiteral("upperPeakRatio"), configuration.upperPeakRatio},
            {QStringLiteral("lowerPeakRatio"), configuration.lowerPeakRatio},
            {QStringLiteral("releaseHoldMs"), configuration.releaseHoldMs},
            {QStringLiteral("releaseStepMs"), configuration.releaseStepMs},
        }},
    };
}

} // namespace

GainProfileStore::GainProfileStore(CommitFunction commit)
    : m_commit(std::move(commit))
{
}

bool GainProfileStore::save(const QString &fileName,
                            const GainConfiguration &configuration,
                            QString *error) const
{
    QString validationError;
    if (!GainConfiguration::validate(configuration, &validationError)) {
        setError(error, validationError);
        return false;
    }

    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Cannot open gain profile for writing: %1").arg(file.errorString()));
        return false;
    }
    const QByteArray contents = QJsonDocument(toJsonObject(configuration)).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size()) {
        setError(error, QStringLiteral("Cannot write gain profile: %1").arg(file.errorString()));
        return false;
    }
    const bool committed = m_commit ? m_commit(file) : file.commit();
    if (!committed) {
        setError(error, QStringLiteral("Cannot commit gain profile: %1").arg(file.errorString()));
        file.cancelWriting();
        return false;
    }
    setError(error, QString());
    return true;
}

std::optional<GainConfiguration> GainProfileStore::load(const QString &fileName, QString *error) const
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot open gain profile for reading: %1").arg(file.errorString()));
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, QStringLiteral("Gain profile JSON parse error: %1").arg(parseError.errorString()));
        return std::nullopt;
    }
    if (!json.isObject()) {
        setError(error, QStringLiteral("Gain profile root must be a JSON object"));
        return std::nullopt;
    }

    const QJsonObject object = json.object();
    if (!validateKeys(object,
                      {QStringLiteral("schemaVersion"),
                       QStringLiteral("channelCount"),
                       QStringLiteral("shifts"),
                       QStringLiteral("automatic"),
                       QStringLiteral("agc")},
                      QStringLiteral("root"),
                      error)) {
        return std::nullopt;
    }
    int schemaVersion = 0;
    if (!readInteger(object.value(QStringLiteral("schemaVersion")), &schemaVersion)) {
        setError(error, QStringLiteral("Gain profile schemaVersion must be an integer"));
        return std::nullopt;
    }
    if (schemaVersion != ProfileSchemaVersion) {
        setError(error, QStringLiteral("Unsupported gain profile schema version"));
        return std::nullopt;
    }
    int channelCount = 0;
    if (!readInteger(object.value(QStringLiteral("channelCount")), &channelCount)) {
        setError(error, QStringLiteral("Gain profile channelCount must be an integer"));
        return std::nullopt;
    }
    if (channelCount != GainConfiguration::ChannelCount) {
        setError(error, QStringLiteral("Gain profile channel count must be 48"));
        return std::nullopt;
    }

    const QJsonValue shiftsValue = object.value(QStringLiteral("shifts"));
    if (!shiftsValue.isArray()) {
        setError(error, QStringLiteral("Gain profile shifts must be an array"));
        return std::nullopt;
    }
    const QJsonArray shifts = shiftsValue.toArray();
    if (shifts.size() != GainConfiguration::ChannelCount) {
        setError(error, QStringLiteral("Gain profile shifts must contain exactly 48 entries"));
        return std::nullopt;
    }
    const QJsonValue automaticValue = object.value(QStringLiteral("automatic"));
    if (!automaticValue.isArray()) {
        setError(error, QStringLiteral("Gain profile automatic modes must be an array"));
        return std::nullopt;
    }
    const QJsonArray automatic = automaticValue.toArray();
    if (automatic.size() != GainConfiguration::ChannelCount) {
        setError(error, QStringLiteral("Gain profile automatic modes must contain exactly 48 entries"));
        return std::nullopt;
    }
    if (!object.value(QStringLiteral("agc")).isObject()) {
        setError(error, QStringLiteral("Gain profile AGC parameters must be an object"));
        return std::nullopt;
    }

    GainConfiguration configuration;
    for (int channel = 0; channel < GainConfiguration::ChannelCount; ++channel) {
        if (!readInteger(shifts.at(channel), &configuration.shifts[channel])) {
            setError(error, QStringLiteral("Gain profile shift at channel %1 must be an integer").arg(channel));
            return std::nullopt;
        }
        if (!automatic.at(channel).isBool()) {
            setError(error, QStringLiteral("Gain profile automatic mode at channel %1 must be boolean").arg(channel));
            return std::nullopt;
        }
        configuration.automatic[channel] = automatic.at(channel).toBool();
    }

    const QJsonObject agc = object.value(QStringLiteral("agc")).toObject();
    if (!validateKeys(agc,
                      {QStringLiteral("upperPeakRatio"),
                       QStringLiteral("lowerPeakRatio"),
                       QStringLiteral("releaseHoldMs"),
                       QStringLiteral("releaseStepMs")},
                      QStringLiteral("agc"),
                      error)) {
        return std::nullopt;
    }
    if (!readNumber(agc.value(QStringLiteral("upperPeakRatio")), &configuration.upperPeakRatio)) {
        setError(error, QStringLiteral("AGC upperPeakRatio must be a finite number"));
        return std::nullopt;
    }
    if (!readNumber(agc.value(QStringLiteral("lowerPeakRatio")), &configuration.lowerPeakRatio)) {
        setError(error, QStringLiteral("AGC lowerPeakRatio must be a finite number"));
        return std::nullopt;
    }
    if (!readInteger(agc.value(QStringLiteral("releaseHoldMs")), &configuration.releaseHoldMs)) {
        setError(error, QStringLiteral("AGC releaseHoldMs must be an integer"));
        return std::nullopt;
    }
    if (!readInteger(agc.value(QStringLiteral("releaseStepMs")), &configuration.releaseStepMs)) {
        setError(error, QStringLiteral("AGC releaseStepMs must be an integer"));
        return std::nullopt;
    }

    QString validationError;
    if (!GainConfiguration::validate(configuration, &validationError)) {
        setError(error, validationError);
        return std::nullopt;
    }
    setError(error, QString());
    return configuration;
}

} // namespace MicArray
