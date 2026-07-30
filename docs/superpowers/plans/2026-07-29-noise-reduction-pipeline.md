# Noise Reduction Pipeline and Marker Drag Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a reorderable post-processing pipeline with LogMMSE and AGC, a tabbed control panel, draggable direction markers in normal and presentation views, persisted configuration, and a +48 dB monitor gain ceiling.

**Architecture:** Keep Wiener suppression as a fixed first-stage processor and run a normalized sequence of band-pass, LogMMSE, and AGC processors inside `NoiseReductionWorker`. Keep all mutable DSP state on the worker thread, represent UI changes as complete settings snapshots, and synchronize two independent video widgets through `NoiseReductionPage`.

**Tech Stack:** C++20, Qt 6 Widgets/Multimedia/Test, existing radix-2 FFT and QSettings infrastructure.

## Global Constraints

- Preserve every pre-existing uncommitted change; do not reset, overwrite, or discard unrelated work.
- Work in the current checkout; do not create a branch and do not commit.
- The suppression node is always first; only band-pass, LogMMSE, and AGC are reorderable.
- Marker drags send exactly once on mouse release in both normal and presentation views.
- LogMMSE and AGC have strict independent bypass modes.
- Monitor gain range is exactly `-60.0..+48.0 dB`.
- Update `doc/Readme.md` and its change record for the delivered behavior.

---

### Task 1: Settings Model and Persistence

**Files:**
- Modify: `src/media/noisereductiontypes.h`
- Modify: `src/media/noisereductiontypes.cpp`
- Modify: `src/app/appsettings.cpp`
- Test: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `enum class NoisePostProcessingNode { BandPass, LogMmse, AutomaticGainControl }`.
- Produces: `LogMmseSettings`, `AutomaticGainControlSettings`, and `QVector<NoisePostProcessingNode> postProcessingOrder`.
- Produces: `normalizedPostProcessingOrder(...)` returning each valid post-processing node exactly once.

- [ ] **Step 1: Write failing settings tests**

Add round-trip assertions for:

```cpp
expected.postProcessingOrder = {
    NoisePostProcessingNode::AutomaticGainControl,
    NoisePostProcessingNode::BandPass,
    NoisePostProcessingNode::LogMmse,
};
expected.logMmse = {
    true, 1024, 0.75, 0.97, 0.91, -24.0
};
expected.automaticGainControl = {
    true, -16.0, 28.0, -18.0, 15.0, 420.0, -1.5
};
expected.monitorGainDb = 48.0;
```

Add invalid-setting coverage proving that duplicate/unknown node keys normalize to the default missing-node order and non-finite/out-of-range algorithm parameters restore their defaults.

- [ ] **Step 2: Run the application settings test and verify failure**

Run:

```powershell
cmake --build build/dev --target test_app
ctest --test-dir build/dev -R test_app --output-on-failure
```

Expected: compilation or assertions fail because the new setting types and keys do not exist.

- [ ] **Step 3: Add settings types and normalization**

Define:

```cpp
enum class NoisePostProcessingNode {
    BandPass,
    LogMmse,
    AutomaticGainControl,
};

struct LogMmseSettings {
    bool enabled = false;
    int fftSize = 512;
    double overlapRatio = 0.5;
    double priorSnrSmoothing = 0.98;
    double noisePowerSmoothing = 0.95;
    double minimumGainDb = -20.0;
    bool operator==(const LogMmseSettings &) const = default;
};

struct AutomaticGainControlSettings {
    bool enabled = false;
    double targetLevelDbfs = -18.0;
    double maximumGainDb = 30.0;
    double maximumAttenuationDb = -20.0;
    double attackMilliseconds = 20.0;
    double releaseMilliseconds = 300.0;
    double limiterLevelDbfs = -1.0;
    bool operator==(const AutomaticGainControlSettings &) const = default;
};
```

Give `NoiseReductionSettings` the default order `{BandPass, LogMmse, AutomaticGainControl}` plus the two settings structs. Normalize order during load and before save. Persist stable string keys `bandPass`, `logMmse`, and `agc`, not localized labels.

- [ ] **Step 4: Sanitize all new values**

Accept only power-of-two FFT sizes from 256 through 2048, overlap `0.0..<1.0`, smoothing values `0.0..<1.0`, finite dB/time values in their UI ranges, and monitor gain `-60.0..+48.0`. Restore individual invalid values from `NoiseReductionSettings{}` rather than rejecting the whole group.

- [ ] **Step 5: Run the focused settings test**

Run the Task 1 commands again and expect `test_app` to pass.

---

### Task 2: LogMMSE Processor

**Files:**
- Create: `src/media/logmmseprocessor.h`
- Create: `src/media/logmmseprocessor.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/media/test_media.cpp`

**Interfaces:**
- Consumes: `LogMmseSettings`.
- Produces:

```cpp
class LogMmseProcessor
{
public:
    bool configure(int sampleRate, const LogMmseSettings &settings);
    void reset();
    QVector<float> process(const QVector<float> &samples);
};
```

- [ ] **Step 1: Write failing processor tests**

Cover:

```cpp
LogMmseProcessor processor;
QVERIFY(processor.configure(48000, LogMmseSettings{}));
const QVector<float> output = processor.process(noisySine);
QCOMPARE(output.size(), noisySine.size());
QVERIFY(std::all_of(output.cbegin(), output.cend(), std::isfinite));
```

Also compare one-block and irregular multi-block processing after flushing equal-length input, verify silence remains finite and near zero, and verify invalid FFT/overlap settings are rejected.

- [ ] **Step 2: Build `test_media` and verify failure**

Run:

```powershell
cmake --build build/dev --target test_media
ctest --test-dir build/dev -R test_media --output-on-failure
```

Expected: compilation fails because `LogMmseProcessor` is missing.

- [ ] **Step 3: Implement streaming STFT state**

Implement a Hann analysis/synthesis window, hop size derived from FFT size and overlap, input/output FIFOs, overlap-add accumulation, and exact input/output length accounting. Reuse `Radix2Fft::transform` for forward and inverse transforms.

- [ ] **Step 4: Implement LogMMSE gain**

For each bin calculate posterior SNR, decision-directed prior SNR, speech-presence-aware smoothed noise power, and:

```cpp
const double v = priorSnr * posteriorSnr / (1.0 + priorSnr);
const double gain =
    priorSnr / (1.0 + priorSnr)
    * std::exp(0.5 * exponentialIntegralE1(v));
```

Clamp gain to the configured minimum and a finite upper bound. Implement a stable positive-domain `E1` approximation for small, medium, and large `v`, and mirror real-input spectrum bins before inverse FFT.

- [ ] **Step 5: Run the focused media tests**

Run the Task 2 commands and expect the new LogMMSE tests to pass.

---

### Task 3: Automatic Gain Control

**Files:**
- Create: `src/media/automaticgaincontrol.h`
- Create: `src/media/automaticgaincontrol.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/media/test_media.cpp`

**Interfaces:**
- Consumes: `AutomaticGainControlSettings`.
- Produces:

```cpp
class AutomaticGainControl
{
public:
    bool configure(int sampleRate,
                   const AutomaticGainControlSettings &settings);
    void reset();
    QVector<float> process(const QVector<float> &samples);
};
```

- [ ] **Step 1: Write failing AGC tests**

Assert that a low-level sine converges toward the configured target without exceeding maximum gain, a loud signal does not exceed the limiter, outputs remain finite, attack is faster than release, and invalid times/limits are rejected.

- [ ] **Step 2: Build `test_media` and verify failure**

Run the Task 2 build/test commands and expect compilation failure for the missing AGC type.

- [ ] **Step 3: Implement RMS envelope and gain smoothing**

Convert attack/release milliseconds to one-pole coefficients at the configured sample rate. Track RMS power, derive target gain in dB, clamp it between maximum attenuation and maximum gain, smooth the gain, and apply a final limiter amplitude of:

```cpp
std::pow(10.0, settings.limiterLevelDbfs / 20.0)
```

- [ ] **Step 4: Run the focused AGC tests**

Run the Task 2 build/test commands and expect all AGC tests to pass.

---

### Task 4: Reorderable Worker Pipeline and Higher Monitor Gain

**Files:**
- Modify: `src/media/noisereductionworker.h`
- Modify: `src/media/noisereductionworker.cpp`
- Test: `tests/media/test_media.cpp`

**Interfaces:**
- Consumes: normalized `NoiseReductionSettings::postProcessingOrder`.
- Consumes: `LogMmseProcessor` and `AutomaticGainControl`.
- Produces: final `NoiseSignalSource::Denoised` after every enabled node in configured order.

- [ ] **Step 1: Write failing worker/pipeline tests**

Add tests proving:

- disabled LogMMSE and AGC are strict bypasses;
- changing `postProcessingOrder` changes the final output for a deterministic signal;
- suppression still runs before all post-processing nodes;
- reconfiguration resets only DSP state and preserves valid settings;
- `pcm16WithGain(samples, 48.0)` applies more gain than `24.0` before saturation.

- [ ] **Step 2: Run `test_media` and verify failure**

Run the Task 2 commands and expect the new assertions to fail.

- [ ] **Step 3: Configure processors and execute the ordered pipeline**

After Wiener suppression, iterate normalized post-processing nodes:

```cpp
for (NoisePostProcessingNode node : m_settings.postProcessingOrder) {
    switch (node) {
    case NoisePostProcessingNode::BandPass:
        if (m_settings.bandPass.enabled)
            output = m_bandPassFilter.process(output);
        break;
    case NoisePostProcessingNode::LogMmse:
        if (m_settings.logMmse.enabled)
            output = m_logMmseProcessor.process(output);
        break;
    case NoisePostProcessingNode::AutomaticGainControl:
        if (m_settings.automaticGainControl.enabled)
            output = m_automaticGainControl.process(output);
        break;
    }
}
```

Reset affected state when the sample rate, relevant parameters, enabled state, or node position changes. Strict bypass means the disabled processor method is not called.

- [ ] **Step 4: Raise monitor ceiling consistently**

Change UI-independent clamps in `configure()` and `pcm16WithGain()` from `24.0` to `48.0`, retaining saturating PCM16 conversion.

- [ ] **Step 5: Run all media tests**

Run the Task 2 commands and expect `test_media` to pass.

---

### Task 5: Draggable Direction Markers

**Files:**
- Modify: `src/ui/noisereductionvideowidget.h`
- Modify: `src/ui/noisereductionvideowidget.cpp`
- Modify: `src/ui/noisereductionpage.h`
- Modify: `src/ui/noisereductionpage.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces video widget signals:

```cpp
void directionDragStarted(NoiseDirectionSelection selection);
void directionDragged(NoiseDirectionSelection selection,
                      const QPointF &normalizedPoint);
void directionDragFinished(NoiseDirectionSelection selection,
                           const QPointF &normalizedPoint);
```

- Produces page handlers that update continuous angles during drag and call `requestControlSend()` only from drag finish.

- [ ] **Step 1: Write failing video-widget tests**

Project known A/B1/B2 targets into a visible test widget. Simulate press, move, and release on a marker and assert:

- the nearest target is selected;
- move emits continuous updates;
- release emits exactly one finish;
- press outside all markers retains `videoPointClicked`;
- hidden markers cannot start a drag.

- [ ] **Step 2: Write failing page integration tests**

Use `QSignalSpy` on `controlSendRequested`. Drag a normal marker and assert zero sends before release and one after release. Open presentation mode, repeat on `noiseReductionPresentationVideo`, and assert the same behavior and synchronized settings.

- [ ] **Step 3: Run UI tests and verify failure**

Run:

```powershell
cmake --build build/dev --target test_dashboard
ctest --test-dir build/dev -R test_dashboard --output-on-failure
```

Expected: missing signals/behavior cause failure.

- [ ] **Step 4: Implement hit testing and drag lifecycle**

Cache or recompute each visible marker screen position with the same projection used by painting. Use a 16-pixel hit radius, choose minimum squared distance, and break equal-distance ties with `m_selection`. Capture mouse movement until release and emit normalized video points only when they remain inside the rendered video rectangle.

- [ ] **Step 5: Synchronize both video widgets in the page**

Route normal and presentation widget drag signals through the same handlers. During drag, update the selected angle spin boxes and call `applySettingsFromUi()` without sending. On finish, apply the final normalized point, refresh both views, and call `requestControlSend()` once.

- [ ] **Step 6: Run UI tests**

Run the Task 5 commands and expect the marker tests to pass.

---

### Task 6: Tabbed Control Panel and Data-Flow Node Editor

**Files:**
- Create: `src/ui/processingnodelistwidget.h`
- Create: `src/ui/processingnodelistwidget.cpp`
- Modify: `src/ui/noisereductionpage.h`
- Modify: `src/ui/noisereductionpage.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces:

```cpp
class ProcessingNodeListWidget : public QListWidget
{
    Q_OBJECT
public:
    void setPostProcessingOrder(
        const QVector<NoisePostProcessingNode> &order);
    QVector<NoisePostProcessingNode> postProcessingOrder() const;
signals:
    void postProcessingOrderChanged(
        const QVector<NoisePostProcessingNode> &order);
};
```

- Consumes/produces all new settings through `NoiseReductionPage::settingsFromUi()` and `restoreSettings()`.

- [ ] **Step 1: Write failing layout and node tests**

Assert that:

- `noiseReductionControlTabs` contains exactly “数据流”“监听”“方向控制”;
- the right panel minimum width is 440 and its initial splitter size is near 520;
- `noiseReductionProcessingNodeList` starts with four nodes;
- the first item is not drag-enabled;
- the other three items can be reordered and emit normalized order;
- selecting each node switches `noiseReductionNodeParameterStack`;
- LogMMSE and AGC enable checkboxes default off;
- monitor gain maximum equals `48.0`.

- [ ] **Step 2: Run UI tests and verify failure**

Run the Task 5 commands and expect missing objects/assertions to fail.

- [ ] **Step 3: Implement the constrained node list**

Use internal move drag/drop. Represent the fixed suppression item separately from the three enum-backed post-processing items. Reject any drop before row 1, then emit the three-node normalized order after a successful drop. Give node items a card-like selected style and update their enabled/parameter summaries.

- [ ] **Step 4: Rebuild the right panel as tabs**

Move existing controls without duplicating them:

- Data flow: node list, parameter `QStackedWidget`, processing status.
- Monitor: monitor enable/source/gain.
- Direction control: target angles/codes, marker controls, mapping, camera, send and presentation actions.

Set the control panel object name, minimum width 440, initial splitter sizes using a preferred 520-pixel panel, and preserve user-resizable splitter behavior.

- [ ] **Step 5: Add LogMMSE and AGC editors**

Create named checkboxes/spin boxes for every confirmed parameter. Bind selected node to its editor page, disable parameter children when a node is bypassed, update node summaries, and include every value/order in `settingsFromUi()`, `restoreSettings()`, persistence, and worker configuration.

- [ ] **Step 6: Block accidental wheel edits**

Install the existing event filter on new numeric algorithm controls as well as all direction/mapping/camera controls, and consume `QEvent::Wheel` for them.

- [ ] **Step 7: Run UI tests**

Run the Task 5 commands and expect `test_dashboard` to pass.

---

### Task 7: Documentation and Full Verification

**Files:**
- Modify: `doc/Readme.md`
- Verify: all files changed by Tasks 1–6

**Interfaces:**
- Produces: maintained behavior documentation and verification evidence.

- [ ] **Step 1: Update behavior documentation**

Document the three control tabs, fixed/reorderable node behavior, default order, LogMMSE and AGC defaults/bypass, marker drag release-send semantics, presentation support, persistence, and the `+48 dB` monitor ceiling. Add a dated change-record entry.

- [ ] **Step 2: Run formatting/diff checks**

Run:

```powershell
git diff --check
git status --short
```

Expected: no whitespace errors; only intentional existing and new source/test/doc changes remain.

- [ ] **Step 3: Build the complete project**

Run:

```powershell
cmake --build build/dev
```

Expected: successful build.

- [ ] **Step 4: Run all C++ tests**

Run:

```powershell
ctest --test-dir build/dev --output-on-failure
```

Expected: every configured C++ test passes.

- [ ] **Step 5: Run Python and mock-device verification**

Run:

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
```

Expected: all Python tests and mock-device self-test pass.

- [ ] **Step 6: Review final diff**

Confirm each requested behavior maps to tests and implementation, no generated files are included, no existing unrelated edit was reverted, and no branch or commit was created.

