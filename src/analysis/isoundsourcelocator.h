#pragma once

#include "analysis/localizationtypes.h"

namespace MicArray {

class ISoundSourceLocator
{
public:
    virtual ~ISoundSourceLocator() = default;

    virtual LocatorDescriptor descriptor() const = 0;
    virtual bool configure(const LocatorConfiguration &configuration,
                           QString *errorMessage) = 0;
    virtual QVector<SoundSourceDirection> locate(const MultiChannelAudioWindow &window) = 0;
};

} // namespace MicArray
