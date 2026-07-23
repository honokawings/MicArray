#pragma once

#include "analysis/localizationtypes.h"

#include <QPointF>

namespace MicArray {

struct ProjectionResult {
    bool valid = false;
    bool inFront = false;
    bool inView = false;
    QPointF normalizedPoint {0.5, 0.5};
    QPointF edgePoint {0.5, 0.5};
};

ProjectionResult projectDirection(const SoundSourceDirection &direction,
                                  const CameraViewSettings &camera);

} // namespace MicArray
