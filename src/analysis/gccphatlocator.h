#pragma once

#include "analysis/isoundsourcelocator.h"
#include "analysis/ispatialresponseprovider.h"
#include "analysis/robustsrpphatscorer.h"

#include <limits>

namespace MicArray {

class GccPhatLocator final : public ISoundSourceLocator,
                             public ISpatialResponseProvider
{
public:
    ~GccPhatLocator() override;

    LocatorDescriptor descriptor() const override;
    bool configure(const LocatorConfiguration &configuration,
                   QString *errorMessage = nullptr) override;
    QVector<SoundSourceDirection> locate(const MultiChannelAudioWindow &window) override;
    void prepareSpatialResponseGrid(const SpatialResponseGrid &grid) override;
    SpatialResponseResult evaluateSpatialResponse(
        const MultiChannelAudioWindow &window,
        const SpatialResponseGrid &grid) override;

private:
    bool ensureWindow(const MultiChannelAudioWindow &window);
    bool windowCacheMatches(
        const MultiChannelAudioWindow &window) const;
    void invalidateWindowCache();

    LocatorConfiguration m_configuration;
    RobustSrpPhatScorer m_scorer;
    RobustSpatialEstimate m_cachedEstimate;
    qint64 m_cachedCenterSample = std::numeric_limits<qint64>::min();
    int m_cachedSampleRate = 0;
    int m_cachedSampleCount = 0;
    int m_cachedChannelCount = 0;
    const float *m_cachedFirstChannelData = nullptr;
    bool m_configured = false;
};

} // namespace MicArray
