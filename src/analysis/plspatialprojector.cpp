#include "analysis/plspatialprojector.h"

#include "analysis/directionprojection.h"

#include <algorithm>
#include <cmath>

namespace MicArray {

LiveSpatialResponse PlSpatialProjector::project(
    const PlSpatialMap &map, const CameraViewSettings &camera, quint64 generation)
{
    LiveSpatialResponse response;
    response.generation = generation;
    response.timestampMicroseconds = map.timestampMicroseconds;
    if (map.sourceMask == 0 || map.values.size() != 1369 || !camera.isValid())
    {
        return response;
    }
    // Cached pixel-to-ray sampling avoids repeated camera trigonometry per map.
    // The display is interpolated; the physical PL scan remains 37 x 37.
    constexpr int Columns = 160;
    constexpr int Rows = 90;
    if (m_samples.isEmpty() || m_camera != camera)
    {
        m_camera = camera;
        m_samples.resize(Columns * Rows);
        for (int row = 0; row < Rows; ++row)
        {
            for (int column = 0; column < Columns; ++column)
            {
                Sample sample;
                const ScanDirection ray = unprojectVideoPoint(
                    {(column + 0.5) / Columns, (row + 0.5) / Rows}, camera);
                if (ray.azimuthDegrees >= -90.0 && ray.azimuthDegrees <= 90.0
                    && ray.elevationDegrees >= -90.0 && ray.elevationDegrees <= 90.0)
                {
                    const double x = (ray.azimuthDegrees + 90.0) / 5.0;
                    const double y = (ray.elevationDegrees + 90.0) / 5.0;
                    const int left = std::min(35, int(std::floor(x)));
                    const int top = std::min(35, int(std::floor(y)));
                    sample.index = top * 37 + left;
                    sample.horizontal = float(x - left);
                    sample.vertical = float(y - top);
                }
                m_samples[row * Columns + column] = sample;
            }
        }
    }
    response.columns = Columns;
    response.rows = Rows;
    response.reliability = map.reliability;
    response.trustedDirection = true;
    response.normalizedValues.resize(m_samples.size());
    for (int cell = 0; cell < m_samples.size(); ++cell)
    {
        const Sample &sample = m_samples.at(cell);
        float value = 0.0f;
        if (sample.index >= 0)
        {
            const float upper = std::lerp(map.values.at(sample.index),
                                         map.values.at(sample.index + 1), sample.horizontal);
            const float lower = std::lerp(map.values.at(sample.index + 37),
                                         map.values.at(sample.index + 38), sample.horizontal);
            value = std::lerp(upper, lower, sample.vertical);
        }
        response.normalizedValues[cell] = value;
    }
    return response;
}

} // namespace MicArray
