#pragma once

#include "analysis/isoundsourcelocator.h"

#include <QHash>

#include <functional>
#include <memory>

namespace MicArray {

using SoundSourceLocatorFactory = std::function<std::unique_ptr<ISoundSourceLocator>()>;

class SoundSourceLocatorRegistry
{
public:
    bool registerFactory(SoundSourceLocatorFactory factory, QString *errorMessage = nullptr);
    QVector<LocatorDescriptor> descriptors() const;
    std::unique_ptr<ISoundSourceLocator> create(const QString &id) const;

private:
    QHash<QString, SoundSourceLocatorFactory> m_factories;
    QHash<QString, LocatorDescriptor> m_descriptors;
};

void registerBuiltInSoundSourceLocators(SoundSourceLocatorRegistry *registry);

} // namespace MicArray
