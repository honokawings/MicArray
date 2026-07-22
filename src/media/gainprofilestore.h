#pragma once

#include "media/channelgainprocessor.h"

#include <functional>
#include <optional>

class QSaveFile;

namespace MicArray {

class GainProfileStore
{
public:
    using CommitFunction = std::function<bool(QSaveFile &)>;

    explicit GainProfileStore(CommitFunction commit = {});

    bool save(const QString &fileName, const GainConfiguration &configuration, QString *error = nullptr) const;
    std::optional<GainConfiguration> load(const QString &fileName, QString *error = nullptr) const;

private:
    CommitFunction m_commit;
};

} // namespace MicArray
