#include "analysis/soundsourcelocatorregistry.h"

#include "analysis/gccphatlocator.h"

#include <algorithm>

namespace MicArray {

bool SoundSourceLocatorRegistry::registerFactory(SoundSourceLocatorFactory factory,
                                                 QString *errorMessage)
{
    if (!factory) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Sound source locator factory is empty");
        }
        return false;
    }

    std::unique_ptr<ISoundSourceLocator> locator = factory();
    if (locator == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Sound source locator factory returned null");
        }
        return false;
    }

    LocatorDescriptor descriptor = locator->descriptor();
    descriptor.id = descriptor.id.trimmed();
    descriptor.displayName = descriptor.displayName.trimmed();
    if (descriptor.id.isEmpty() || descriptor.displayName.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Sound source locator id and display name are required");
        }
        return false;
    }
    if (m_factories.contains(descriptor.id)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Sound source locator '%1' is already registered")
                                .arg(descriptor.id);
        }
        return false;
    }

    m_factories.insert(descriptor.id, std::move(factory));
    m_descriptors.insert(descriptor.id, std::move(descriptor));
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

QVector<LocatorDescriptor> SoundSourceLocatorRegistry::descriptors() const
{
    QVector<LocatorDescriptor> result;
    result.reserve(m_descriptors.size());
    for (const LocatorDescriptor &descriptor : m_descriptors) {
        result.append(descriptor);
    }
    std::sort(result.begin(), result.end(), [](const LocatorDescriptor &left,
                                               const LocatorDescriptor &right) {
        const int byName = QString::localeAwareCompare(left.displayName, right.displayName);
        return byName == 0 ? left.id < right.id : byName < 0;
    });
    return result;
}

std::unique_ptr<ISoundSourceLocator> SoundSourceLocatorRegistry::create(const QString &id) const
{
    const auto factory = m_factories.constFind(id);
    return factory == m_factories.cend() ? nullptr : (*factory)();
}

void registerBuiltInSoundSourceLocators(SoundSourceLocatorRegistry *registry)
{
    if (registry == nullptr) {
        return;
    }
    registry->registerFactory([] {
        return std::make_unique<GccPhatLocator>();
    });
}

} // namespace MicArray
