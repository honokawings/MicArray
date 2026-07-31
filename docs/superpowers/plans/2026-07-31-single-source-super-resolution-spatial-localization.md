# Single-Source Super-Resolution Spatial Localization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the four-baseline, linearly interpolated SRP-PHAT path with a coherent multi-baseline scorer that returns one continuous single-source peak shared by localization and a genuinely super-resolved heatmap.

**Architecture:** Add a UI-independent `RobustSrpPhatScorer` that owns channel-quality analysis, displacement-grouped coherent PHAT correlations, cached grid steering, robust normalization, and continuous peak refinement. Keep `GccPhatLocator` as the stable plugin-facing adapter, propagate refined-peak metadata through the live worker, and render the Hessian-derived local likelihood at sub-grid coordinates over the measured SRP background.

**Tech Stack:** C++20, Qt 6.5+ Core/Gui/Widgets/Concurrent, existing radix-2 FFT, Qt Test, CMake/Ninja, pure CPU.

## Global Constraints

- Only one source in the array front hemisphere is in scope.
- Use the existing 6×8, 5 cm planar geometry and configured speed of sound.
- No neural model, GPU backend, platform-specific SIMD requirement, or third-party runtime.
- Preserve the stable locator ID `gcc-phat-tdoa` and the source-level extension interfaces.
- Realtime Enhanced Release median for localization plus `640×360` heatmap must be at most `100 ms`.
- High Precision Release median for localization plus `640×360` heatmap must be at most `200 ms`.
- Invalid, silent, diffuse, or insufficient-channel input must not produce a high-confidence sharp peak.
- Do not create a branch, stage files, or commit; leave changes unstaged on `master`.
- Every production behavior is introduced only after its focused test has been observed failing for the expected reason.
- Update `doc/Readme.md` and its change record with the delivered behavior and measured verification.

---

### Task 1: Add stable spatial-mode and refined-peak data contracts

**Files:**
- Modify: `src/analysis/localizationtypes.h`
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Test: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `SpatialResolutionMode`.
- Produces: `LocatorConfiguration::spatialResolutionMode`.
- Produces: `LiveLocalizationSettings::spatialResolutionMode`.
- Produces: refined-peak metadata on `SpatialResponseResult` and `LiveSpatialResponse`.
- Consumes: existing `AppSettings::liveLocalization()` and `saveLiveLocalization(...)`.

- [ ] **Step 1: Add failing persistence/default tests**

Add a test slot named `persistsSpatialResolutionMode()` and extend invalid-live-settings coverage:

```cpp
void AppTest::persistsSpatialResolutionMode()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("settings.ini"));
    AppSettings settings(path);

    LiveLocalizationSettings expected = settings.liveLocalization();
    QCOMPARE(expected.spatialResolutionMode,
             SpatialResolutionMode::RealtimeEnhanced);
    expected.spatialResolutionMode = SpatialResolutionMode::HighPrecision;
    settings.saveLiveLocalization(expected);

    QCOMPARE(AppSettings(path).liveLocalization().spatialResolutionMode,
             SpatialResolutionMode::HighPrecision);
}
```

Write raw value `liveLocalization/spatialResolutionMode=unknown` in
`invalidLiveLocalizationFieldsFallBackIndependently()` and assert only this field falls back to
`RealtimeEnhanced`.

- [ ] **Step 2: Run the focused app test and verify RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_app --output-on-failure
```

Expected: compile failure because `SpatialResolutionMode` and
`spatialResolutionMode` do not exist.

- [ ] **Step 3: Add the contracts**

Add to `localizationtypes.h`:

```cpp
enum class SpatialResolutionMode {
    RealtimeEnhanced,
    HighPrecision,
};

struct LocatorConfiguration {
    // existing fields...
    SpatialResolutionMode spatialResolutionMode =
        SpatialResolutionMode::RealtimeEnhanced;
};

struct SpatialResponseResult {
    QVector<float> values;
    double reliability = 0.0;
    bool hasRefinedPeak = false;
    ScanDirection refinedPeak;
    double azimuthStandardDeviationDegrees = 0.0;
    double elevationStandardDeviationDegrees = 0.0;
    double peakToSidelobeRatio = 0.0;
};
```

Mirror the same five metadata fields in `LiveSpatialResponse`, add
`Q_DECLARE_METATYPE(MicArray::SpatialResolutionMode)`, and add this field to
`LiveLocalizationSettings`:

```cpp
SpatialResolutionMode spatialResolutionMode =
    SpatialResolutionMode::RealtimeEnhanced;
```

- [ ] **Step 4: Persist the mode with independent sanitization**

Use the exact string keys `realtime-enhanced` and `high-precision`. Add helpers in
`appsettings.cpp`:

```cpp
QString spatialResolutionModeKey(SpatialResolutionMode mode);
bool spatialResolutionModeFromKey(const QString &key,
                                  SpatialResolutionMode *mode);
```

Read `liveLocalization/spatialResolutionMode`; on an unknown value assign
`SpatialResolutionMode::RealtimeEnhanced` without changing other fields. Save the canonical key
from `saveLiveLocalization(...)`.

- [ ] **Step 5: Run the app test and verify GREEN**

Run the Task 1 commands again. Expected: `test_app` passes.

- [ ] **Step 6: Review the Task 1 diff**

Run:

```powershell
git diff --check
git diff -- src/analysis/localizationtypes.h src/app/appsettings.h src/app/appsettings.cpp tests/app/test_app.cpp
```

Confirm no production algorithm behavior has changed yet.

---

### Task 2: Create the scorer boundary, geometry grouping, and input-quality gate

**Files:**
- Create: `src/analysis/robustsrpphatscorer.h`
- Create: `src/analysis/robustsrpphatscorer.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Consumes: `LocatorConfiguration`, `MultiChannelAudioWindow`, `SpatialResponseGrid`.
- Produces: `RobustSpatialEstimate`.
- Produces: `RobustSrpPhatScorer::configure`, `prepareGrid`, `prepareWindow`,
  `estimate`, and `reset`.

- [ ] **Step 1: Add failing scorer validation tests**

Include `analysis/robustsrpphatscorer.h`, add test slots
`groupsDefaultArrayAcrossTwoDimensions()` and
`rejectsInsufficientValidChannels()`:

```cpp
void AudioAnalysisTest::groupsDefaultArrayAcrossTwoDimensions()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    QVERIFY(scorer.baselineGroupCount() >= 70);
    QVERIFY(scorer.realtimeBaselineGroupCount() >= 24);
    QVERIFY(scorer.realtimeBaselineGroupCount() <= 32);
}

void AudioAnalysisTest::rejectsInsufficientValidChannels()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    MultiChannelAudioWindow window = syntheticPlaneWave(13.4, -7.2);
    for (int channel = 0; channel < 17; ++channel) {
        window.channels[channel].fill(0.0f);
    }
    QVERIFY(!scorer.prepareWindow(window));
    QVERIFY(!scorer.estimate().hasDirection);
}
```

The two count accessors are diagnostic read-only API used by tests and runtime status.

- [ ] **Step 2: Build `test_analysis` and verify RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
```

Expected: compile failure because the scorer files and class do not exist.

- [ ] **Step 3: Define the scorer result and public API**

Create `robustsrpphatscorer.h` with:

```cpp
struct RobustSpatialEstimate {
    bool hasDirection = false;
    SoundSourceDirection direction;
    QVector<float> gridValues;
    double reliability = 0.0;
    double peakToSidelobeRatio = 0.0;
    double azimuthStandardDeviationDegrees = 0.0;
    double elevationStandardDeviationDegrees = 0.0;
    int validChannelCount = 0;
    int validBaselineGroupCount = 0;
};

class RobustSrpPhatScorer
{
public:
    bool configure(const LocatorConfiguration &configuration,
                   QString *errorMessage = nullptr);
    void prepareGrid(const SpatialResponseGrid &grid);
    bool prepareWindow(const MultiChannelAudioWindow &window);
    RobustSpatialEstimate estimate(
        const SpatialResponseGrid *grid = nullptr);
    void reset();

    int baselineGroupCount() const;
    int realtimeBaselineGroupCount() const;
};
```

Keep pair/group/cache structures private. Do not include UI or settings headers.

- [ ] **Step 4: Implement deterministic displacement grouping**

Enumerate every unordered microphone pair. Canonicalize its displacement into the positive
half-plane and quantize each component to integer nanometres:

```cpp
qint64 quantizedMeters(double value)
{
    return qRound64(value * 1.0e9);
}
```

Use `(dx,dy,dz)` as a sortable group key. For the default array, select realtime groups by
round-robin over 12 angle buckets and three normalized length buckets, sorting candidates in each
bucket by descending pair count and then by displacement key. Stop at 32 groups.

- [ ] **Step 5: Implement channel-quality gating**

Validate sample rate, channel count, sample count, and finite samples. Compute Hann-windowed energy
per channel and its median. Assign zero quality outside `[-24,+18] dB` relative to the median and a
continuous `0.25..1.0` quality inside the range:

```cpp
const double normalizedDeviation =
    std::min(1.0, std::abs(relativeDb) / 24.0);
quality = 1.0 - 0.75 * normalizedDeviation;
```

Return `false` if fewer than 32 channels survive. Store valid-channel count for `estimate()`.

- [ ] **Step 6: Register the new files in CMake**

Add `src/analysis/robustsrpphatscorer.cpp` to `MICARRAY_SOURCES` and
`src/analysis/robustsrpphatscorer.h` to `MICARRAY_HEADERS`.

- [ ] **Step 7: Run focused tests and verify GREEN**

Build and run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

Expected: the new geometry and insufficient-channel tests pass; existing analysis tests remain
green.

---

### Task 3: Build coherent PHAT group correlations and cached cubic grid scanning

**Files:**
- Modify: `src/analysis/robustsrpphatscorer.h`
- Modify: `src/analysis/robustsrpphatscorer.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Consumes: geometry groups and channel qualities from Task 2.
- Produces: one coherent weighted GCC per active displacement group.
- Produces: prepared steering lags and robust-normalized grid response.

- [ ] **Step 1: Add a failing off-grid spatial-response test**

Add `producesMultiBaselineResponseNearestOffGridSource()`:

```cpp
void AudioAnalysisTest::producesMultiBaselineResponseNearestOffGridSource()
{
    RobustSrpPhatScorer scorer;
    LocatorConfiguration configuration;
    configuration.microphones = createDefaultArrayGeometry();
    QVERIFY(scorer.configure(configuration));
    const SpatialResponseGrid grid = fieldOfViewGrid(64, 36);
    scorer.prepareGrid(grid);
    QVERIFY(scorer.prepareWindow(syntheticPlaneWave(17.35, -8.65)));
    const RobustSpatialEstimate result = scorer.estimate(&grid);

    QCOMPARE(result.gridValues.size(), grid.directions.size());
    QVERIFY(result.validBaselineGroupCount >= 24);
    QVERIFY(std::all_of(result.gridValues.cbegin(),
                        result.gridValues.cend(),
                        [](float value) {
                            return std::isfinite(value)
                                && value >= 0.0f && value <= 1.0f;
                        }));
    const int peak = static_cast<int>(std::distance(
        result.gridValues.cbegin(),
        std::max_element(result.gridValues.cbegin(),
                         result.gridValues.cend())));
    QVERIFY(angularDistanceDegrees(grid.directions.at(peak),
                                   {17.35, -8.65}) < 2.5);
}
```

Add the test helper:

```cpp
double angularDistanceDegrees(const ScanDirection &first,
                              const ScanDirection &second)
{
    const double cosine = std::clamp(
        static_cast<double>(QVector3D::dotProduct(
            directionFromAngles(first.azimuthDegrees,
                                first.elevationDegrees),
            directionFromAngles(second.azimuthDegrees,
                                second.elevationDegrees))),
        -1.0, 1.0);
    return qRadiansToDegrees(std::acos(cosine));
}
```

- [ ] **Step 2: Run the focused test and verify RED**

Run `test_analysis producesMultiBaselineResponseNearestOffGridSource` through CTest by temporarily
using the normal full target if direct QtTest invocation lacks runtime DLL paths.

Expected: assertion failure because `gridValues` is empty.

- [ ] **Step 3: Compute one-sided phase-normalized channel spectra**

For each valid channel:

1. subtract the mean;
2. apply Hann window;
3. zero-pad from `windowSamples` to `2 * windowSamples`;
4. run `Radix2Fft::transform(..., false)`;
5. retain bins spanning `100..8150 Hz` so the 150 Hz tapers around the
   `250..8000 Hz` passband can be computed.

Cache the complex unit phase and channel quality; reject non-finite FFT output.

- [ ] **Step 4: Aggregate coherent PHAT spectra per group**

For every group and retained bin compute:

```cpp
weightedPhaseSum += firstQuality * secondQuality
    * firstUnitPhase * std::conj(secondUnitPhase);
weightSum += firstQuality * secondQuality;
```

Then:

```cpp
const std::complex<double> average = weightedPhaseSum / weightSum;
const double coherence = std::abs(average);
const double reliability =
    smoothStep(0.15, 0.65, coherence) * bandTaper(frequencyHz);
groupSpectrum[bin] = reliability > 0.0
    ? reliability * average / coherence
    : std::complex<double>();
```

Fill the conjugate negative-frequency bins, inverse FFT each group, and divide the resulting GCC by
the sum of retained reliability weights. Mark a group valid only when at least 15% of passband bins
have nonzero reliability.

- [ ] **Step 5: Precompute steering lags for the requested grid**

For each valid front-hemisphere direction:

```cpp
lagSamples = dot(group.displacement, direction)
    * sampleRate / soundSpeedMetersPerSecond;
```

Cache wrapped integer center index and fractional part for every active group/direction. Invalidate
the cache if dimensions or direction values change.

- [ ] **Step 6: Implement four-point cubic interpolation and row-parallel scanning**

Use Catmull-Rom cubic interpolation:

```cpp
double cubic(double y0, double y1, double y2, double y3, double fraction);
```

Scan Realtime Enhanced grids with selected groups and High Precision grids with all groups. Use the
existing pattern of `QThreadPool(max(1, idealThreadCount - 1))` and disjoint row-aligned output
ranges. Keep reduction and normalization after `blockingMap` deterministic.

- [ ] **Step 7: Implement robust percentile normalization**

Copy scores for selection, compute median and the `99.5%` element with `std::nth_element`, reject a
range below `1e-9`, then:

```cpp
value = std::pow(std::clamp((value - median) / (upper - median),
                            0.0, 1.0),
                 0.75);
```

During RED diagnostics, exact clipping saturated the top 0.5% of cells and hid the true argmax.
The delivered implementation therefore maps the formula above to `[0,0.98]` and reserves
`[0.98,1]` for monotonic interpolation from `P99.5` to the actual maximum. This preserves robust
background scaling and unique peak ordering.

- [ ] **Step 8: Run focused and existing analysis tests**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

Expected: new off-grid grid test passes and all values are finite in `[0,1]`.

---

### Task 4: Add global seeding, all-baseline sinc refinement, uncertainty, and robustness

**Files:**
- Modify: `src/analysis/robustsrpphatscorer.h`
- Modify: `src/analysis/robustsrpphatscorer.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Consumes: coherent GCC group cache from Task 3.
- Produces: `RobustSpatialEstimate::direction`, uncertainty, PSR, and reliability.
- Produces: real all-baseline response replacement around the refined peak.

- [ ] **Step 1: Add failing sub-degree direction tests**

Add `refinesSingleSourceBelowHalfDegree()` over:

```cpp
const QVector<ScanDirection> expected {
    {3.37, -2.41},
    {27.65, 14.35},
    {-36.25, -18.75},
    {58.45, 22.15},
};
```

For each direction, prepare the synthetic window, call `estimate()`, require
`hasDirection`, and assert spherical angular error `< 0.5°`, positive finite standard deviations,
and `reliability > 0.45`.

- [ ] **Step 2: Verify the sub-degree test fails for the expected reason**

Build and run `test_analysis`. Expected: `hasDirection` is false because continuous refinement is
not implemented.

- [ ] **Step 3: Implement the global hemisphere seed**

Build a cached front-hemisphere seed grid:

- Realtime Enhanced: azimuth/elevation step `5°`.
- High Precision: azimuth/elevation step `2°`.
- Both ranges: `-90°..90°`; skip directions with `z < 0`.

Score the seed grid with cubic interpolation. Use its maximum as the initial direction even when a
video grid is absent or does not include the source.

- [ ] **Step 4: Implement eight-tap Hann-windowed sinc scoring**

Add:

```cpp
double scoreDirectionSinc(const ScanDirection &direction,
                          const QVector<int> &groupIndices) const;
```

For each group, evaluate four samples on each side of the theoretical fractional lag:

```cpp
weight = sinc(distance)
    * (0.5 + 0.5 * std::cos(std::numbers::pi * distance / 4.0));
```

Normalize by the interpolation weight sum and then by the number of valid groups.

- [ ] **Step 5: Implement deterministic 3×3 pattern refinement**

Start from `max(2°, estimatedGridSpacing)` capped at `6°`. Evaluate center plus eight neighboring
directions using all valid groups. Move to the best neighbor; otherwise halve the step. Stop at
`0.05°`, two improvements below `1e-5`, or 40 iterations. Clamp elevation and reject directions
behind the planar array.

- [ ] **Step 6: Estimate curvature, PSR, and reliability**

Use samples at `±0.25°` and `±0.5°` to fit the 2×2 negative Hessian. If it is positive definite,
derive standard deviations from its inverse after scaling by the local residual floor. Otherwise
leave deviations zero.

Calculate peak-to-sidelobe ratio by excluding seed-grid directions within `5°`, and combine:

```cpp
reliability = std::pow(
    channelCoverage * coherentCoverage * psrScore * curvatureScore,
    0.25);
```

Clamp every factor and the result to `[0,1]`. Set `hasDirection` only when the result is finite and
the minimum channel/group/frequency coverage gates pass.

- [ ] **Step 7: Add and verify abnormal-channel robustness**

Add `toleratesFourCorruptedMicrophonesWithMultiBaselineScoring()`. Replace channels
`0, 11, 26, 47` with independent deterministic noise and assert angular error `<1.5°`.

Run once before improving weighting and observe the intended precision/reliability failure. Adjust
only group coherence and channel-quality calculations—not the expected direction or limit—until it
passes.

- [ ] **Step 8: Add and verify noise-plus-reflection robustness**

Create a deterministic helper that adds:

- white noise for `5 dB` SNR;
- a `0.35` amplitude reflection delayed by `3 ms`;
- a reflected direction offset by `25°` azimuth and `10°` elevation.

Add `localizesThroughNoiseAndSingleReflection()` and assert angular error `<2.0°` and finite
confidence. Observe RED before tuning coherent thresholds, then make the scorer pass without
special-casing the test angle.

- [ ] **Step 9: Add ambiguity and invalid-input rejection**

Add `rejectsDiffuseAndInsufficientSpatialEvidence()` using 48 independent noise channels and
`remainsFiniteForNarrowbandInput()` using a pure tone. Diffuse noise must have
`hasDirection == false` or `reliability < configuration.confidenceThreshold`; narrowband output
must contain no non-finite values and must not report a high-confidence sharp peak.

- [ ] **Step 10: Recompute the local map patch with all groups**

For grid cells within `max(6°, three local grid cells)` of the refined direction, replace the fast
score with the eight-tap, all-group score before robust normalization. Add an assertion that the
discrete grid maximum is the cell with smallest angular distance to the refined peak.

- [ ] **Step 11: Run all analysis tests**

Build and run `test_analysis`; inspect reported errors and reliability instead of relaxing
thresholds. Expected: all Task 4 precision and robustness limits pass.

---

### Task 5: Make `GccPhatLocator` use one cached continuous peak for both APIs

**Files:**
- Modify: `src/analysis/gccphatlocator.h`
- Modify: `src/analysis/gccphatlocator.cpp`
- Modify: `src/analysis/audioanalysisworker.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Consumes: `RobustSrpPhatScorer`.
- Produces: stable `ISoundSourceLocator` and `ISpatialResponseProvider` behavior.
- Preserves: locator ID `gcc-phat-tdoa`.

- [ ] **Step 1: Add a failing shared-peak test**

Add `localizationAndHeatmapShareContinuousPeak()`:

```cpp
GccPhatLocator locator;
LocatorConfiguration configuration;
configuration.microphones = createDefaultArrayGeometry();
QVERIFY(locator.configure(configuration));
const MultiChannelAudioWindow window =
    syntheticPlaneWave(19.37, -6.82);
const QVector<SoundSourceDirection> located = locator.locate(window);
const SpatialResponseGrid grid = fieldOfViewGrid(64, 36);
const SpatialResponseResult spatial =
    locator.evaluateSpatialResponse(window, grid);

QCOMPARE(located.size(), 1);
QVERIFY(spatial.hasRefinedPeak);
QVERIFY(angularDistanceDegrees(
    {located.first().azimuthDegrees,
     located.first().elevationDegrees},
    spatial.refinedPeak) < 0.1);
```

Expected RED: old locator and response do not expose or share a refined peak.

- [ ] **Step 2: Replace duplicated locator internals with the scorer**

Remove old pair TDOA, four-baseline group, steering, and FFT caches from
`GccPhatLocator`. Store:

```cpp
RobustSrpPhatScorer m_scorer;
RobustSpatialEstimate m_cachedEstimate;
MultiChannelAudioWindowKey m_cachedWindowKey;
```

Keep only configuration, temporal direction smoothing, and API adaptation.

- [ ] **Step 3: Implement order-independent cache use**

On either API call, compare `centerSample`, sample rate/count, channel count, and first-channel data
address. A new key calls `prepareWindow()` and clears the cached estimate. `locate()` requests an
estimate without a display grid. `evaluateSpatialResponse()` requests the prepared display grid;
the scorer must reuse spectra/GCC and the same refined direction.

- [ ] **Step 4: Apply temporal smoothing and confidence consistently**

Apply the configured smoothing factor once to the continuous direction. Store the smoothed
direction back into cached metadata so both APIs report it. If reliability is below
`confidenceThreshold`, `locate()` returns empty and `SpatialResponseResult::hasRefinedPeak` is
false while diagnostic grid values may remain.

- [ ] **Step 5: Populate `SpatialResponseResult` metadata**

Copy normalized grid values, reliability, refined direction, deviations, and PSR from the scorer.
Update descriptor text without changing its ID.

- [ ] **Step 6: Select High Precision for offline analysis**

In `AudioAnalysisWorker::rebuildLocator()`, assign:

```cpp
configuration.spatialResolutionMode =
    SpatialResolutionMode::HighPrecision;
```

Live mode will be supplied by Task 7.

- [ ] **Step 7: Run locator and registry regressions**

Run `test_analysis`. Expected: original synthetic localization, registry, silent-window, grid-cache,
and shared-peak tests all pass.

---

### Task 6: Propagate normalized metadata and render a truthful sub-grid peak

**Files:**
- Modify: `src/analysis/livelocalizationworker.cpp`
- Modify: `src/ui/livelocalizationview.h`
- Modify: `src/ui/livelocalizationview.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: normalized `SpatialResponseResult` and refined metadata.
- Produces: `LiveSpatialResponse` without second min/max normalization.
- Produces: scalar-field heatmap rendering with continuous Hessian-derived local likelihood.

- [ ] **Step 1: Add a failing worker metadata/normalization test**

Extend the live-worker test with a fake spatial provider result containing values
`{0.0f, 0.25f, 0.75f, 1.0f}`, refined peak `{17.3,-6.4}`, deviations, and PSR. Assert emitted
`LiveSpatialResponse` retains those values and metadata exactly on the first frame.

Expected RED: metadata is not propagated and values are re-min/max-normalized.

- [ ] **Step 2: Remove redundant worker min/max scaling**

Validate incoming values are finite and within `[0,1]`, copy them, apply only the configured
temporal EMA, and propagate every metadata field. Clear smoothed values when the grid, mode,
locator, timestamp sequence, or active state changes.

- [ ] **Step 3: Expose a deterministic render helper**

In `LiveLocalizationView`, add a private implementation plus test-facing public method:

```cpp
QImage renderHeatmapForTest(const QSize &size) const;
```

Production `paintEvent()` calls the same helper. The helper renders at clamped dimensions:

```cpp
width = std::clamp(std::max(256, response.columns), 256, 640);
height = std::clamp(std::max(144, response.rows), 144, 360);
```

Preserve target aspect ratio by using the requested test/paint size to choose the internal
dimensions within those limits.

- [ ] **Step 4: Render scalar values before color mapping**

For every internal pixel, bilinearly sample the response scalar grid, then apply the existing
blue-to-red HSV palette. Do not scale the already-colored low-resolution image.

- [ ] **Step 5: Add the Hessian-derived local model**

Project the refined direction and its azimuth/elevation standard-deviation offsets through the
current pinhole camera. If all points are valid and standard deviations are positive, evaluate an
elliptical Gaussian-equivalent quadratic likelihood only inside three standard deviations:

```cpp
local = std::exp(-0.5 * (dx * dx + dy * dy));
scalar = std::max(scalar, local);
```

Cap `local` at the measured response peak. Skip this path when `hasRefinedPeak` is false,
reliability is nonpositive, projection fails, or the ellipse is degenerate.

- [ ] **Step 6: Add failing then passing render-center tests**

Construct a `64×36` response whose discrete peak is intentionally offset from metadata. Set a
valid refined peak and deviations, render `256×144`, and assert the hottest output pixel is within
one pixel of `projectDirection(refinedPeak, camera)`. Add a second case with
`hasRefinedPeak=false` and assert no artificial sub-grid center is introduced.

- [ ] **Step 7: Run analysis and UI tests**

Build and run `test_analysis` and `test_ui`. Expected: worker propagation, render center, existing
marker, calibration, and page lifecycle tests pass.

---

### Task 7: Add the persisted realtime/high-precision UI mode

**Files:**
- Modify: `src/analysis/livelocalizationworker.cpp`
- Modify: `src/ui/livelocalizationpage.h`
- Modify: `src/ui/livelocalizationpage.cpp`
- Test: `tests/app/test_app.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: `LiveLocalizationSettings::spatialResolutionMode`.
- Produces: `LocatorConfiguration::spatialResolutionMode`.
- Produces: combo object `liveSpatialResolutionModeCombo`.

- [ ] **Step 1: Add a failing page persistence/reconfigure test**

Create a page with stored `HighPrecision`, find:

```cpp
auto *combo = page.findChild<QComboBox *>(
    QStringLiteral("liveSpatialResolutionModeCombo"));
```

Assert it selects `HighPrecision`, switch to `RealtimeEnhanced`, and verify
`AppSettings::liveLocalization()` updates. Use `QSignalSpy` on
`configureWorkerRequested` and require one reconfigure emission.

- [ ] **Step 2: Verify RED**

Build `test_ui`. Expected: combo lookup returns null.

- [ ] **Step 3: Add the UI control**

Add a `QComboBox *m_spatialResolutionModeCombo`. Populate:

```cpp
addItem(QStringLiteral("实时增强（10 FPS）"),
        static_cast<int>(SpatialResolutionMode::RealtimeEnhanced));
addItem(QStringLiteral("高精度（约 5 FPS）"),
        static_cast<int>(SpatialResolutionMode::HighPrecision));
```

Place it after the locator row and before display mode. Restore by item data, persist on change,
clear the view response, rebuild the grid, and emit `configureWorkerRequested(m_settings)`.

- [ ] **Step 4: Pass the mode into the live locator**

Include mode in `LiveLocalizationWorker::configure()` change detection and in
`rebuildLocator()`:

```cpp
configuration.spatialResolutionMode =
    m_settings.spatialResolutionMode;
```

Changing mode must rebuild the locator and clear stream/spatial smoothing state.

- [ ] **Step 5: Verify settings and UI GREEN**

Run `test_app` and `test_ui`. Expected: persistence, invalid fallback, combo selection,
reconfigure, and existing 1366×768 layout tests pass.

---

### Task 8: Enforce realtime and high-precision CPU budgets

**Files:**
- Modify: `src/analysis/robustsrpphatscorer.*`
- Modify: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Consumes: complete scorer and locator pipeline.
- Produces: two measurable performance gates with warm cache.

- [ ] **Step 1: Split the performance test by mode**

Refactor the existing maximum-grid benchmark into a helper that runs five warmed iterations for a
given `SpatialResolutionMode`. Add:

```cpp
void evaluatesRealtimeSuperResolutionGridWithinCpuBudget();
void evaluatesHighPrecisionGridWithinCpuBudget();
```

Require 230,400 finite values, a refined peak, and Release medians of `100 ms` and `200 ms`.
Use `1500 ms` for both in Debug.

- [ ] **Step 2: Run both Release tests and record RED measurements**

Build:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Release --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Release -R test_analysis --output-on-failure
```

Expected: if either budget fails, the log identifies localization, full-grid, and local-refinement
durations separately.

- [ ] **Step 3: Optimize only measured hotspots**

Apply these optimizations in order, rerunning the Release benchmark after each:

1. cache channel spectra and group GCCs across `locate()`/heatmap calls;
2. store steering indices/fractions in structure-of-arrays form;
3. scan contiguous row blocks with disjoint output pointers;
4. compute only retained one-sided frequency bins before Hermitian completion;
5. reuse scratch FFT/group buffers instead of reallocating each frame;
6. avoid sinc evaluation outside the local refinement/patch.

Do not reduce requested grid size, skip grid directions, loosen precision tests, or replace the
measured local response with an image filter.

- [ ] **Step 4: Verify both performance modes GREEN**

Run five warmed samples twice to avoid accepting a one-off result. Record both median series in the
test output and later in `doc/Readme.md`.

---

### Task 9: Documentation, full verification, and completion audit

**Files:**
- Modify: `doc/Readme.md`
- Review: all files changed by Tasks 1–8

**Interfaces:**
- Documents: algorithm, modes, failure gates, super-resolution rendering, tests, and measured CPU
  performance.

- [ ] **Step 1: Update maintained behavior documentation**

In the realtime localization section, replace the four-baseline description with:

- all-displacement grouping and realtime 32-group selection;
- coherent frequency reliability and channel-quality gates;
- cubic full-grid scan and all-group sinc continuous refinement;
- shared continuous peak for location and heatmap;
- robust percentile normalization and Hessian-derived sub-grid rendering;
- Realtime Enhanced/High Precision mode behavior and persistence.

Correct the older noise-reduction direction mapping sentence that still states elevation defaults
as `-45°..45°`; actual defaults are `-90°..90°`.

- [ ] **Step 2: Add the 2026-07-31 change record**

Record affected modules, the single-source super-resolution behavior, exact accuracy/robustness
tests, Release medians, full test commands, mock self-test, and `git diff --check`.

- [ ] **Step 3: Run formatting and focused verification**

```powershell
git diff --check
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R "test_analysis|test_app|test_ui" --output-on-failure
```

- [ ] **Step 4: Run complete C++ regression**

```powershell
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
```

Expected: all configured C++ tests pass.

- [ ] **Step 5: Run Python and mock-device regression**

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
```

Expected: all Python tests and built-in self-test pass.

- [ ] **Step 6: Re-run Release performance gates**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Release --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Release -R test_analysis --output-on-failure
```

Expected: Realtime Enhanced median ≤`100 ms`; High Precision median ≤`200 ms`.

- [ ] **Step 7: Audit every acceptance criterion against evidence**

Create a temporary checklist in the work log and verify:

1. shared continuous peak test;
2. `<0.5°` clean off-grid accuracy tests;
3. `<1.5°` four-corrupted-channel test;
4. `<2.0°` 5 dB noise plus reflection test;
5. diffuse/silence/insufficient-channel rejection;
6. sub-grid render-center test;
7. both persisted modes and invalid fallback;
8. Release performance tests;
9. full regression and documentation.

Do not claim completion if any item lacks direct passing evidence.

- [ ] **Step 8: Leave the workspace unstaged and report status**

Run:

```powershell
git status --short
git diff --stat
git diff --check
```

Do not stage or commit. Report modified/new files, exact verification results, measured accuracy,
robustness, and performance, plus any real-device validation that remains outside automation.
