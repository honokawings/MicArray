#pragma once

#include "control/controlvalues.h"
#include "media/noisereductiontypes.h"

namespace MicArray {

class DirectionControlMapper
{
public:
    static qint16 azimuthCode(double degrees,
                             const DirectionControlMapping &mapping);
    static qint16 elevationCode(double degrees,
                               const DirectionControlMapping &mapping);
    static double azimuthDegrees(qint16 code,
                                 const DirectionControlMapping &mapping);
    static double elevationDegrees(qint16 code,
                                   const DirectionControlMapping &mapping);
    static ContinuousDirectionTarget clampDirection(
        const ContinuousDirectionTarget &target,
        const DirectionControlMapping &mapping);
    static ControlValues controlValues(
        const ContinuousDirectionTarget &enhancedA,
        const ContinuousDirectionTarget &suppressionB1,
        const ContinuousDirectionTarget &suppressionB2,
        const DirectionControlMapping &mapping);
};

} // namespace MicArray
