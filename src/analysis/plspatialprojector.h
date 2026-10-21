#pragma once

#include "analysis/localizationtypes.h"
#include "media/plspatialmap.h"

namespace MicArray {

class PlSpatialProjector
{
public:
    LiveSpatialResponse project(const PlSpatialMap &map,
                                const CameraViewSettings &camera,
                                quint64 generation);

private:
    struct Sample
    {
        int index = -1;
        float horizontal = 0.0f;
        float vertical = 0.0f;
    };
    CameraViewSettings m_camera;
    QVector<Sample> m_samples;
};

} // namespace MicArray
