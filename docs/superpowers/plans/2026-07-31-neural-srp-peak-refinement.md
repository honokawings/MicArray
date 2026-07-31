# Neural SRP Peak Refinement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a trained, physics-gated neural postprocessor that sharpens the single-source SRP main peak and measurably improves sub-grid direction accuracy without adding runtime dependencies.

**Architecture:** `RobustSrpPhatScorer` remains the physical source of truth. It samples a fixed `9×9` sinc-SRP patch around its continuous peak, passes normalized patch and diagnostics into an embedded `NeuralSrpPeakRefiner`, accepts only bounded corrections consistent with the physical Hessian and score, then sharpens only the accepted main-peak neighborhood. A deterministic optional C++ trainer produces the embedded model and independent evaluation report.

**Tech Stack:** C++20, Qt 6 Core/Concurrent/Test/Widgets, CMake, embedded `std::array<float>` model weights, deterministic C++ Adam trainer; no ONNX/Torch/Python/GPU/runtime download.

## Global Constraints

- Keep the existing locator ID `gcc-phat-tdoa` and single-source interface stable.
- Runtime model parameter count must be at most `8192` float values and embedded data at most `32 KiB`.
- Neural correction must be at most one patch cell and at most `1.5°`.
- Neural inference must never turn an invalid physical result into a valid direction.
- Model failure, invalid input, invalid output, low confidence, or disabled configuration must exactly fall back to the physical path.
- Neural overhead median must be at most `2 ms`; Release total medians remain at most `100 ms` realtime and `200 ms` high precision for `640×360`.
- Preserve current generation, timestamp rollback, temporal-objective consistency, thread, and UI cache behavior.
- Update `doc/Readme.md` and its change record.
- Work directly in the current workspace; do not create a branch, stage, or commit unless the user later requests it.

---

## File Map

**Create**

- `src/analysis/neuralsrppeakrefiner.h`: model input/output types, model view, validation, and inference API.
- `src/analysis/neuralsrppeakrefiner.cpp`: normalization, dense forward pass, activations, model checksum, and bounded output decoding.
- `src/analysis/neuralmodeldata.h`: generated versioned weights, normalization constants, metadata, and checksum.
- `tools/train_neural_srp_peak.cpp`: deterministic acoustic dataset generation, Adam training, evaluation, atomic model/report export, and `--self-test`.
- `doc/neural-srp-peak-v1-training-report.json`: held-out accuracy, safety, and training provenance.

**Modify**

- `CMakeLists.txt`: production sources and `train_neural_srp_peak EXCLUDE_FROM_ALL`.
- `src/analysis/localizationtypes.h`: neural configuration and diagnostic result fields.
- `src/analysis/robustsrpphatscorer.h/.cpp`: local sinc patch sampling, neural gating/fusion, covariance update, and grid sharpening.
- `src/analysis/gccphatlocator.cpp`: propagate neural diagnostics with the cached same-window result.
- `src/analysis/audioanalysisworker.cpp`: pass offline neural settings.
- `src/analysis/livelocalizationworker.cpp`: rebuild/epoch behavior and diagnostics propagation.
- `src/app/appsettings.h/.cpp`: offline/live persistence and validation.
- `src/ui/audioanalysispage.h/.cpp`: offline neural enable/strength controls.
- `src/ui/livelocalizationpage.h/.cpp`: realtime neural enable/strength controls and generation reset.
- `src/ui/livelocalizationview.cpp`: consume already-sharpened response without a second independent center or smoothing stage.
- `tests/analysis/test_audioanalysis.cpp`: inference, safety, direction, heatmap, performance, and worker consistency tests.
- `tests/app/test_app.cpp`: persistence/default/invalid-value tests.
- `tests/ui/test_dashboard.cpp`: controls and generation tests.
- `doc/Readme.md`: behavior, limitations, training procedure, performance, tests, and change record.

---

### Task 1: Embedded Neural Runtime

**Files:**

- Create: `src/analysis/neuralsrppeakrefiner.h`
- Create: `src/analysis/neuralsrppeakrefiner.cpp`
- Create: `src/analysis/neuralmodeldata.h`
- Modify: `CMakeLists.txt`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**

- Produces:

```cpp
struct NeuralSrpPeakInput {
    static constexpr int PatchSize = 9;
    static constexpr int PatchValueCount = 81;
    QVector<float> normalizedPatch;
    std::array<float, 12> diagnostics {};
};

struct NeuralSrpPeakOutput {
    bool valid = false;
    double deltaAzimuthCells = 0.0;
    double deltaElevationCells = 0.0;
    double azimuthSigmaScale = 1.0;
    double elevationSigmaScale = 1.0;
    double correlationResidual = 0.0;
    double confidence = 0.0;
};

class NeuralSrpPeakRefiner {
public:
    bool modelValid() const;
    NeuralSrpPeakOutput infer(
        const NeuralSrpPeakInput &input) const;
};
```

- Consumes: generated arrays from `neuralmodeldata.h`.

- [ ] **Step 1: Write failing inference and validation tests**

Add test slots:

```cpp
void neuralPeakModelRejectsInvalidInputs();
void neuralPeakModelProducesBoundedFiniteOutput();
void neuralPeakModelMatchesGoldenForwardResult();
```

The invalid-input test supplies 80 and 82 values, constant values, one `NaN`,
one `Inf`, and a non-finite diagnostic. Every result must have `valid == false`.

The bounded-output test supplies the committed model's documented golden
`9×9` patch and verifies:

```cpp
QVERIFY(output.valid);
QVERIFY(std::abs(output.deltaAzimuthCells) <= 1.0);
QVERIFY(std::abs(output.deltaElevationCells) <= 1.0);
QVERIFY(output.azimuthSigmaScale >= 0.35
        && output.azimuthSigmaScale <= 1.0);
QVERIFY(output.elevationSigmaScale >= 0.35
        && output.elevationSigmaScale <= 1.0);
QVERIFY(std::abs(output.correlationResidual) <= 0.35);
QVERIFY(output.confidence >= 0.0
        && output.confidence <= 1.0);
```

The golden test compares all six decoded values to constants exported in the
training report with tolerance `1e-6`.

- [ ] **Step 2: Run the tests and verify RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_analysis --output-on-failure
```

Expected: compile failure because `neuralsrppeakrefiner.h` does not exist.

- [ ] **Step 3: Implement model view, checksum, forward pass, and decoding**

Use `double` accumulators with float weights. Forward computation is:

```cpp
h1[j] = std::tanh(b1[j]
    + sum_i(normalizedInput[i] * w1[j * InputCount + i]));
h2[j] = std::tanh(b2[j]
    + sum_i(h1[i] * w2[j * Hidden1 + i]));
raw[j] = b3[j]
    + sum_i(h2[i] * w3[j * Hidden2 + i]);
```

Decode with:

```cpp
delta = std::tanh(raw);
sigmaScale = 0.35 + 0.65 * sigmoid(raw);
correlationResidual = 0.35 * std::tanh(raw);
confidence = sigmoid(raw);
```

Reject input unless size is 81, every value and diagnostic is finite, patch
range exceeds `1e-5`, model dimensions equal `93/48/24/6`, parameter count is
at most 8192, and the FNV-1a checksum equals the embedded checksum.

Use an initial deterministic seed model in `neuralmodeldata.h` only to bring
up the runtime tests; Task 3 replaces it with trained weights and golden values.

- [ ] **Step 4: Run focused tests and verify GREEN**

Run the Task 1 tests by name and then full `test_analysis`.

- [ ] **Step 5: Checkpoint without staging or committing**

Run `git diff --check` and inspect `git status --short`.

---

### Task 2: Physical Local Patch and Safe Neural Fusion

**Files:**

- Modify: `src/analysis/localizationtypes.h`
- Modify: `src/analysis/robustsrpphatscorer.h`
- Modify: `src/analysis/robustsrpphatscorer.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**

- Adds to `LocatorConfiguration`:

```cpp
bool neuralPeakEnhancementEnabled = true;
double neuralPeakEnhancementStrength = 0.30;
```

- Adds to `RobustSpatialEstimate`:

```cpp
bool neuralRefinementApplied = false;
double neuralConfidence = 0.0;
double neuralCorrectionDegrees = 0.0;
double neuralAzimuthSigmaScale = 1.0;
double neuralElevationSigmaScale = 1.0;
```

- Produces for the trainer:

```cpp
struct SrpLocalPeakPatch {
    QVector<float> normalizedValues;
    std::array<float, 12> diagnostics {};
    double azimuthStepDegrees = 0.0;
    double elevationStepDegrees = 0.0;
    double centerPhysicalScore = 0.0;
    double dynamicRange = 0.0;
};

std::optional<SrpLocalPeakPatch> sampleLocalPeakPatch(
    const RobustSpatialEstimate &physicalEstimate) const;
```

- [ ] **Step 1: Write failing local-patch and bypass tests**

Add:

```cpp
void samplesNormalizedNineByNineSincPatch();
void disabledNeuralPeakEnhancementExactlyMatchesPhysicalPath();
void neuralPeakEnhancementNeverCreatesDirectionFromInvalidEvidence();
void neuralCorrectionStaysInsidePhysicalSafetyBounds();
```

Required assertions:

- Patch size is 81, finite, in `[0,1]`, range greater than `1e-5`.
- Step formula equals `clamp(sigma / 2.5, 0.25, 1.5)`.
- Two separately configured scorers with neural disabled produce identical
  direction, covariance, reliability, and grid values.
- Silence, diffuse noise, marginal narrowband, and fewer than 32 valid channels
  retain `hasDirection == false`.
- Applied correction is at most one cell, `1.5°`, and `2.5σ`.

- [ ] **Step 2: Run tests and verify RED**

Expected: compile failure for missing patch and diagnostic interfaces.

- [ ] **Step 3: Factor local patch normalization**

Inside the scorer:

1. Calculate azimuth/elevation steps from physical standard deviations.
2. Evaluate all 81 directions with `scoreDirectionSinc(..., m_validGroupIndices)`.
3. Normalize using median and P95.
4. Populate diagnostics in the exact Task 1 order.
5. Return `nullopt` for invalid range, angle, or non-finite values.

Do not use `m_preparedGrid` or display-grid dimensions.

- [ ] **Step 4: Add neural inference and gating after physical refinement**

Keep a copy named `physicalEstimate`. Accept the neural output only when:

```cpp
configuration.neuralPeakEnhancementEnabled
&& physicalEstimate.hasDirection
&& output.valid
&& output.confidence >= 0.55
&& correctionDegrees <= 1.5
&& correctionMahalanobisSquared <= 6.25
&& correctedScore >= 0.97 * physicalScore
```

Blend cells with:

```cpp
const double blend = std::clamp(
    configuration.neuralPeakEnhancementStrength
    * output.confidence
    * physicalEstimate.reliability,
    0.0, 1.0);
```

Clamp final angles to the front hemisphere and update covariance scales and
correlation only after every gate succeeds. Cache the fused estimate so
`locate()` and `evaluateSpatialResponse()` share it.

- [ ] **Step 5: Run focused and full analysis tests**

Expected: all Task 2 tests and existing direction/grid/Hessian consistency
tests pass.

- [ ] **Step 6: Checkpoint without staging or committing**

Run `git diff --check` and inspect the focused diff.

---

### Task 3: Deterministic Trainer and Real Embedded Model

**Files:**

- Create: `tools/train_neural_srp_peak.cpp`
- Create: `doc/neural-srp-peak-v1-training-report.json`
- Replace generated values: `src/analysis/neuralmodeldata.h`
- Modify: `CMakeLists.txt`
- Test: trainer `--self-test` and `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**

- Command:

```powershell
build\<kit>\train_neural_srp_peak.exe `
  --seed 20260731 `
  --train-samples 4096 `
  --validation-samples 512 `
  --test-samples 512 `
  --output-header src\analysis\neuralmodeldata.h `
  --output-report doc\neural-srp-peak-v1-training-report.json
```

- `--self-test` trains on 32/8 samples, repeats the run twice, compares every
  weight bitwise, exercises pair-export rollback, reloads the exported golden
  sample, and compares trainer/runtime output within `1e-6`. Normal export
  rejects independent test sets smaller than 256 samples.

- [ ] **Step 1: Add failing trainer target and self-test contract**

Add `train_neural_srp_peak EXCLUDE_FROM_ALL` to CMake and a small Qt Test or
command-level self-test entry point. First configure/build must fail because
the source does not exist.

- [ ] **Step 2: Implement deterministic acoustic sample generation**

Copy no production approximation. Use `RobustSrpPhatScorer` with neural disabled
and generate 48-channel windows from the actual 6×8 geometry:

```cpp
source = broadband noise or 4-12 randomized tones;
channel = fractionalDelay(source, dot(microphone, direction) / 343.0);
channel += whiteNoise + bandLimitedNoise + delayedReflection;
```

Randomize source direction over azimuth/elevation `[-75,+75]`, reflection delay
`2..25 ms`, bandwidth and amplitude mismatch. Use a CPU-count-independent fixed
8-shard generator, deterministic 50%/20%/20%/10% strata for 0/1/2/4 corrupt
channels, and independently permuted 80%/10%/10% multi-tone/dense-broadband/
band-limited strata; increase reflection gain and noise range with stress.

Store only the 93 input values and six target values after generation so Adam
epochs do not rerun FFT/SRP.

- [ ] **Step 3: Implement training and losses**

Use a fixed-order single-threaded Adam update with:

- Direction target: true residual divided by patch step and clipped to one cell.
- Width target: target sigma `0.5` cells.
- Confidence target: 1 only for valid dynamic range, valid physical evidence,
  and true angular residual within `1.5°`.
- Direction Huber delta `0.15`, width Huber delta `0.1`, confidence BCE.
- Early stopping on validation direction median with patience 20; never inspect
  the test split until final export.

- [ ] **Step 4: Implement atomic export and report**

Header export includes dimensions, arrays, input means/scales, golden input,
golden decoded output, seed, sample counts, and checksum.

Report JSON includes:

```json
{
  "modelVersion": 1,
  "seed": 20260731,
  "trainSamples": 4096,
  "validationSamples": 512,
  "testSamples": 512,
  "parameterCount": 5838,
  "gridArgmaxMedianDegrees": 0.0,
  "physicalMedianDegrees": 0.0,
  "neuralMedianDegrees": 0.0,
  "physicalP95Degrees": 0.0,
  "neuralP95Degrees": 0.0,
  "neuralAppliedSamples": 0,
  "checksum": "0000000000000000"
}
```

Do not replace existing outputs unless training, validation, production-path
independent test, checksum, overall thresholds, and every stress-bucket safety
threshold pass. Stage both artifacts first and restore the prior pair if the
second atomic replacement fails.

- [ ] **Step 5: Run trainer self-test**

Build the optional target and run `--self-test`. Expected: deterministic pass,
atomic-export pass, runtime parity pass.

- [ ] **Step 6: Train and export the real model**

Run the full command. Quantize every candidate to the same `float` representation
used by the runtime, then production-evaluate it with an independent continuous
previous window and the default `0.35` scorer smoothing. Compare the candidate
against the quantized warm start on the validation split; replace the warm start
only when the candidate reduces the production-validation median by at least 5%.
Do not use the test split for checkpoint selection. On the final independent test,
accept a reproducible minimum 0.3% physical-peak improvement only when the separate
30% grid improvement, P95 safety thresholds, and every minimum-24-sample bucket
also pass.

The final production-equivalent 512-sample evaluation is:
grid `0.364232°`, physical `0.213979°`, neural `0.213062°`, physical improvement
`0.43%`, and P95 `1.684582° → 1.684582°`; 396/512 corrections pass all runtime
gates. Edge, reflection, signal-kind, and 0/1/2/4-corrupt-channel buckets are
reported separately, each retains at least 24 samples, and each must stay within
`+0.05°`. The validation comparison selected the stable quantized warm start;
the independent test remained unopened until that choice.

- [ ] **Step 7: Rebuild runtime tests with generated model**

Update golden assertions from the generated header/report and run full
`test_analysis`.

- [ ] **Step 8: Checkpoint without staging or committing**

Run `git diff --check`; inspect that only source model/report files, never build
artifacts, appear in `git status --short`.

---

### Task 4: Neural Heatmap Sharpening and Result Propagation

**Files:**

- Modify: `src/analysis/localizationtypes.h`
- Modify: `src/analysis/robustsrpphatscorer.cpp`
- Modify: `src/analysis/gccphatlocator.cpp`
- Modify: `src/analysis/livelocalizationworker.cpp`
- Modify: `src/ui/livelocalizationview.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**

- Add identical neural diagnostics to `SpatialResponseResult` and
  `LiveSpatialResponse`.
- Scorer modifies normalized grid values only when
  `neuralRefinementApplied == true`.

- [ ] **Step 1: Write failing heatmap tests**

Add:

```cpp
void neuralHeatmapPeakMatchesFusedDirection();
void neuralHeatmapShrinksHalfPowerArea();
void neuralHeatmapPreservesValuesOutsideThreeSigma();
void liveWorkerPropagatesNeuralDiagnostics();
```

For a deterministic held-out source:

- Peak-to-final-direction distance is at most `0.25°`.
- Count of cells `>= 0.5 * maximum` shrinks at least 25%.
- Every cell outside `3σ` differs by at most `1e-6`.
- All output values remain finite and in `[0,1]`.

- [ ] **Step 2: Run tests and verify RED**

Expected: missing diagnostics and unchanged half-power area.

- [ ] **Step 3: Apply bounded local sharpening**

After robust normalization, for cells inside the neural `3σ` ellipse:

```cpp
shoulder = std::pow(original, gamma)
    * std::exp(-0.5 * (gamma - 1.0) * mahalanobisSquared);
kernel = peakAmplitude * std::exp(-0.5 * mahalanobisSquared);
enhanced = std::clamp(std::max(shoulder, kernel), 0.0, 1.0);
```

Choose `gamma` from configured strength and neural confidence. Use final fused
direction as kernel center. Outside `3σ`, leave the original float untouched.

- [ ] **Step 4: Propagate same-window diagnostics**

Copy fields through `GccPhatLocator`, `LiveLocalizationWorker`, and the view.
Do not add worker EMA, coordinate smoothing, or a UI-side independent peak.
The existing generation filters continue to reject stale responses.

- [ ] **Step 5: Run analysis and UI tests**

Run `ctest -R "test_analysis|test_ui"` and verify existing covariance/roll/cache
tests remain green.

- [ ] **Step 6: Checkpoint without staging or committing**

Run `git diff --check`.

---

### Task 5: Persistent Offline and Realtime Controls

**Files:**

- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Modify: `src/analysis/audioanalysisworker.cpp`
- Modify: `src/analysis/livelocalizationworker.cpp`
- Modify: `src/ui/audioanalysispage.h`
- Modify: `src/ui/audioanalysispage.cpp`
- Modify: `src/ui/livelocalizationpage.h`
- Modify: `src/ui/livelocalizationpage.cpp`
- Test: `tests/app/test_app.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**

- Stable keys:

```text
audioAnalysis/neuralPeakEnhancementEnabled
audioAnalysis/neuralPeakEnhancementStrength
liveLocalization/neuralPeakEnhancementEnabled
liveLocalization/neuralPeakEnhancementStrength
```

- Defaults: `true`, `0.30`; valid strength `[0,1]`.

- UI object names:

```text
audioAnalysisNeuralPeakEnhancementCheck
audioAnalysisNeuralPeakEnhancementStrengthSpin
liveLocalizationNeuralPeakEnhancementCheck
liveLocalizationNeuralPeakEnhancementStrengthSpin
```

- [ ] **Step 1: Write failing persistence tests**

Extend round-trip tests with non-default values and invalid-field tests with
strength `NaN`, `-0.1`, and `1.1`. Each invalid strength falls back independently
to `0.30`; valid neighboring fields remain loaded.

- [ ] **Step 2: Write failing UI and generation tests**

Verify both pages expose the named controls. On the live page, toggling either
control must emit one new `spatialGridRequested` generation, clear the current
view, persist the setting, and emit `configureWorkerRequested`.

- [ ] **Step 3: Run app/UI tests and verify RED**

Expected: missing fields and controls.

- [ ] **Step 4: Implement settings validation**

Use existing finite-range helpers. Store bool directly and strength as double.
Do not couple neural validity to unrelated camera or grid settings.

- [ ] **Step 5: Add controls and worker configuration**

Use a checkbox plus `QDoubleSpinBox` with range `[0,1]`, step `0.05`, two
decimals, and blocked wheel behavior consistent with adjacent numeric controls.
Offline changes reconfigure and reanalyze the current position. Live changes
call `startNewProcessingEpoch()` before queued worker configuration.

Pass both settings into `LocatorConfiguration`. Include them in worker
`locatorChanged` detection so the scorer and temporal state rebuild.

- [ ] **Step 6: Run app/UI/analysis tests**

Expected: persistence, controls, generation, offline reanalysis, and worker
same-window tests pass.

- [ ] **Step 7: Checkpoint without staging or committing**

Run `git diff --check`.

---

### Task 6: Independent Accuracy, Robustness, and Performance Gates

**Files:**

- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `doc/neural-srp-peak-v1-training-report.json`

**Interfaces:**

- Adds deterministic held-out test helpers that use seeds never present in the
  training command.

- [ ] **Step 1: Add held-out aggregate accuracy test**

Add:

```cpp
void neuralPeakEnhancementImprovesHeldOutLocalization();
```

Generate at least 256 cases across clean, noise, reflection, bandwidth, edge,
and 1/2/4-corrupt-channel buckets. Run physical disabled and neural enabled
scorers on identical windows. Record grid argmax, physical, and neural angular
errors. Assert:

```cpp
neuralMedian <= 0.95 * physicalMedian;
neuralMedian <= 0.70 * gridArgmaxMedian;
neuralP95 <= physicalP95 + 0.05;
```

Also assert every named bucket has at least 24 accepted cases and does not
increase median error by more than `0.05°`.

- [ ] **Step 2: Add false-positive parity test**

Run at least 16 diffuse-noise seeds, silence, marginal bandwidth, and
insufficient-channel windows through both configurations. Neural valid-direction
count must be exactly equal to physical valid-direction count.

- [ ] **Step 3: Add neural overhead benchmark**

Warm both paths, alternate measurement order, collect at least 21 samples, and
compare medians. Release assertion:

```cpp
neuralMedianMilliseconds - physicalMedianMilliseconds <= 2;
```

Keep existing total `640×360` `100/200 ms` gates.

- [ ] **Step 4: Run Release analysis gates twice**

Build Release `test_analysis`, run the focused aggregate/false-positive/overhead
tests twice, then full Release `test_analysis`. Record both medians in the report.

- [ ] **Step 5: Checkpoint without staging or committing**

Run `git diff --check`.

---

### Task 7: Documentation, Full Verification, and Review

**Files:**

- Modify: `doc/Readme.md`
- Modify: `docs/superpowers/specs/2026-07-31-neural-srp-peak-refinement-design.md`
- Modify: `docs/superpowers/plans/2026-07-31-neural-srp-peak-refinement.md`

- [ ] **Step 1: Update maintained documentation**

Document model structure, embedded deployment, local patch, gates, settings,
training command, report path, performance, simulation-only evidence, physical
limitations, and rollback behavior. Add a 2026-07-31 change-record row.

- [ ] **Step 2: Run final Debug build and all CTest targets**

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
```

Expected: build succeeds and 6/6 tests pass.

- [ ] **Step 3: Run final trainer and tool checks**

```powershell
build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug\train_neural_srp_peak.exe --self-test
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
```

Expected: trainer self-test, 45 Python tests, and mock self-test pass.

- [ ] **Step 4: Run final Release gates**

Build and run Release `test_analysis`; capture realtime/high-precision total
medians and neural overhead in the maintained documentation and report.

- [ ] **Step 5: Run repository audit**

```powershell
git diff --check
git status --short
git branch --show-current
git log -3 --oneline
```

Expected: no whitespace errors, branch `master`, no generated build artifacts
in status, and only intended source/docs/tests/tools changes.

- [ ] **Step 6: Request independent code review**

Reviewer must inspect model validation, array bounds, checksum, training/test
split isolation, acceptance gates, physical fallback equality, worker
generation, UI same-window consistency, and performance evidence. Fix every
Critical or Important finding and rerun affected plus full tests.

- [ ] **Step 7: Preserve workspace without integration actions**

Keep the current `master` worktree as-is. Do not stage, commit, push, merge, or
create a branch unless the user explicitly changes the existing instruction.
