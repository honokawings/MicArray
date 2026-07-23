#pragma once

#include "analysis/localizationtypes.h"

namespace MicArray {

class ISpatialResponseProvider
{
public:
    virtual ~ISpatialResponseProvider() = default;

    virtual SpatialResponseResult evaluateSpatialResponse(
        const MultiChannelAudioWindow &window,
        const SpatialResponseGrid &grid) = 0;
};

} // namespace MicArray
