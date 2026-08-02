# Noise Monitor Buffer and Multi-Source Recording Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add smooth buffered noise-reduction monitoring, selectable noise data-flow recording sources, separate playable WAV files, and a noise-page recording shortcut.

**Architecture:** `NoiseReductionWorker` captures selected DSP node outputs once per input frame and publishes a typed recording frame. The existing `AudioRecorder` remains the only asynchronous recording state machine and manages one writer for combined output or one writer per track for separate output. A pure byte FIFO isolates monitor jitter from `QAudioSink`, while `MainWindow` owns recording-source selection and keeps the noise worker active when a noise-flow session is running.

**Tech Stack:** C++20, Qt 6 Widgets/Multimedia/Core/Test, CMake, WAV extensible PCM, QThread queued signals.

## 实施状态（2026-08-02）

任务 1 至 6 的实现已集成到当前工作区；任务 7 的文档与全量验证结果记录在同目录 `task-7-report.md`。独立最终审查已由 controller 完成且无 Critical/Important；本次 Minor fix wave 后由 controller 进行 scoped re-review。

## Global Constraints

- Preserve every existing unstaged workspace change; do not create a branch, stage, or commit.
- Keep array recording based on gain-adjusted 48-channel payloads and preserve its default combined-file behavior.
- A session selects either array channels or noise data-flow stages, never both.
- Noise stages are Suppression, BandPass, LogMmse, and AutomaticGainControl; bypassed nodes emit the unchanged previous-stage audio.
- Monitor buffer default is 250 ms, legal range 50..1000 ms, maximum queued duration twice the target.
- Keep all queues bounded and never block network, normal playback, localization, video, or the UI thread.
- Update `doc/Readme.md` and its change record.

---

## File Map

- Create `src/media/audiomonitorbuffer.h/.cpp`: bounded PCM16 FIFO with prebuffer threshold.
- Modify `src/media/noisereductiontypes.h/.cpp`: stage enum, stage names, recording-frame value types, monitor-buffer setting.
- Modify `src/media/noisereductionworker.h/.cpp`: monitor queue pumping and selected-stage capture.
- Modify `src/media/multichannelwavwriter.h/.cpp`: source/file-layout metadata in manifests.
- Modify `src/media/audiorecorder.h/.cpp`: noise-stage enqueue path and one-or-many writer orchestration.
- Modify `src/app/appsettings.cpp`: monitor-buffer persistence and sanitization.
- Modify `src/ui/noisereductionpage.h/.cpp`: buffer UI, background recording requirement, frame forwarding, shortcut button.
- Modify `src/ui/mainwindow.h/.cpp`: source/layout UI, recording settings, recorder routing, shortcut integration.
- Modify `CMakeLists.txt`: compile the new monitor buffer.
- Modify `tests/media/test_media.cpp`, `tests/app/test_app.cpp`, `tests/ui/test_dashboard.cpp`: behavioral regression coverage.
- Modify `doc/Readme.md`: maintained behavior and change record.

### Task 1: Recording and Noise-Stage Value Types

**Files:**

- Modify: `src/media/noisereductiontypes.h`
- Modify: `src/media/noisereductiontypes.cpp`
- Modify: `src/media/multichannelwavwriter.h`
- Test: `tests/media/test_media.cpp`

**Interfaces:**

- Produce `enum class NoiseReductionDataFlowStage { Suppression, BandPass, LogMmse, AutomaticGainControl };`
- Produce `struct NoiseReductionStageSamples { NoiseReductionDataFlowStage stage; QVector<float> samples; };`
- Produce `struct NoiseReductionRecordingFrame { QVector<NoiseReductionStageSamples> stages; int sampleRate; quint32 timestampSeconds; quint32 timestampMicros; };`
- Produce `noiseReductionDataFlowStageName()` and `noiseReductionDataFlowStageKey()`.
- Extend `RecordingSettings` with `RecordingInputSource`, `RecordingFileLayout`, `noiseReductionStages`, `trackKeys`, and `trackNames` after existing fields so current aggregate initializers remain valid.

- [ ] Add a media test with literal expected keys `suppression`, `bandpass`, `logmmse`, `agc` and Chinese display names; mutation caught: swapped or unstable stage-to-file mapping.
- [ ] Build `test_media` and verify RED because the stage types/functions do not exist.
- [ ] Add the enums, structs, names, keys, default array input, and default single-multichannel layout; register value types required by queued signals.
- [ ] Rebuild and run the focused test; verify GREEN.

### Task 2: Persistent Bounded Monitor Buffer

**Files:**

- Create: `src/media/audiomonitorbuffer.h`
- Create: `src/media/audiomonitorbuffer.cpp`
- Modify: `src/media/noisereductiontypes.h`
- Modify: `src/app/appsettings.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/media/test_media.cpp`
- Test: `tests/app/test_app.cpp`

**Interfaces:**

- Produce `AudioMonitorBuffer::configure(int sampleRate, int targetMilliseconds)`.
- Produce `append(const QByteArray &) -> qsizetype` returning dropped oldest bytes.
- Produce `readyToStart()`, `availableBytes()`, `peek(qsizetype)`, `discard(qsizetype)`, and `clear()`.
- Add `NoiseReductionSettings::monitorBufferMilliseconds = 250`.

- [ ] Add pure media tests at 48 kHz mono PCM16: 250 ms target equals 24000 bytes, startup is false at 23998 and true at 24000, `peek/discard` preserves the suffix, and appending beyond 48000 bytes drops the oldest even number of bytes.
- [ ] Add app tests proving default 250, round-trip 400, and independent fallback for 49, 1001, and non-finite values.
- [ ] Build focused tests and verify RED because the class/member/key are absent.
- [ ] Implement the FIFO with a byte-array plus read offset, compacting only when needed; align every capacity/drop/discard operation to two bytes.
- [ ] Add `noiseReduction/monitorBufferMilliseconds` load/save and sanitize to 50..1000.
- [ ] Rebuild and run focused media/app tests; verify GREEN.

### Task 3: Monitor Queue and Data-Flow Capture in the Worker

**Files:**

- Modify: `src/media/noisereductionworker.h`
- Modify: `src/media/noisereductionworker.cpp`
- Test: `tests/media/test_media.cpp`

**Interfaces:**

- Add slot `setRecordingStages(const QVector<NoiseReductionDataFlowStage> &stages)`.
- Add signal `recordingFrameReady(const NoiseReductionRecordingFrame &frame)`.
- Replace `applyPostProcessing(QVector<float>)` with a helper that applies actual node order and captures only requested post-node samples.
- Add worker members `AudioMonitorBuffer m_monitorBuffer`, selected stage set/vector, and one-shot overflow warning state.

- [ ] Add a real-worker test with deterministic CH1/B1/B2 payload and all post nodes bypassed; request all stages and assert one frame contains four 200-sample tracks, identical at bypass boundaries, with literal sample rate/timestamp.
- [ ] Add a second test with a constant non-zero input, Voice bandpass enabled, AGC enabled, and post order `AGC -> BandPass -> LogMMSE`; request AGC then BandPass and assert the returned IDs keep that requested order, AGC samples differ from suppression samples, and BandPass samples differ from AGC samples. Mutation caught: capturing before a node, ignoring the configured order, or returning every unrequested stage.
- [ ] Add a test that clears requested stages and asserts no recording frame is emitted.
- [ ] Run focused tests and verify RED because the slot/signal are absent.
- [ ] Implement capture around the existing single DSP pass; do not alter plot history or final monitor selection.
- [ ] In monitor output, enqueue PCM first, wait for `readyToStart()`, create a sink buffer sized to the target, write at most `bytesFree()`, and discard only the written prefix; clear on source/rate/enable/buffer changes.
- [ ] Rebuild and run the worker/monitor tests; verify GREEN.

### Task 4: Multi-Writer Layout and Noise-Frame Recording

**Files:**

- Modify: `src/media/multichannelwavwriter.h`
- Modify: `src/media/multichannelwavwriter.cpp`
- Modify: `src/media/audiorecorder.h`
- Modify: `src/media/audiorecorder.cpp`
- Test: `tests/media/test_media.cpp`

**Interfaces:**

- Add `AudioRecorder::enqueueNoiseReductionFrame(const NoiseReductionRecordingFrame &frame)`.
- `AudioRecorder::Worker` owns `QVector<std::unique_ptr<MultiChannelWavWriter>>`.
- Add packing helper that maps selected stage samples to internal channels 0..N-1 using `round(clamp(sample) * 32768) << 8` and fills a standard 38400-byte payload.
- Extend `RecordingSummary`/`RecordingStatistics` with aggregated audio-file count while preserving `outputPath` as the first manifest for compatibility.

- [ ] Add writer tests that a noise frame with literal `0.25` and `-0.5` becomes raw WAV samples `0x00200000` and `0xffc00000` in a two-channel combined file.
- [ ] Add separate-layout test selecting suppression and AGC: assert files end in `_suppression.wav` and `_agc.wav`, both headers report one channel, both contain correct samples, and aggregate file count is two.
- [ ] Add manifest assertions for `inputSource`, `fileLayout`, `trackKey`, and `trackName`.
- [ ] Add invalid-frame tests for wrong sample rate, missing selected stage, and sample count other than 200; mutation caught: writing misaligned partial tracks.
- [ ] Run focused tests and verify RED because noise enqueue and layout fields do not exist.
- [ ] Extend writer manifest metadata without changing existing header/data selection paths.
- [ ] Implement writer-set startup, payload fan-out, aggregate summaries, all-writers stop on any failure, and gain metadata update for every array writer.
- [ ] Implement noise-frame queue validation and asynchronous append using the existing byte/frame limits.
- [ ] Run focused and all media tests; verify GREEN, especially existing segmentation, collision, short-write, queue-overflow, and gain tests.

### Task 5: Recording Source and Layout UI

**Files:**

- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**

- Add `recordingSourceModeCombo`, `recordingSourceStack`, four `recordingNoiseStage_*` buttons, and `recordingFileLayoutCombo` object names.
- Add helpers `selectedNoiseRecordingStages()`, `recordingSettingsFromUi()`, and `recordingConfigurationIsValid()`.
- Track the immutable active session input in `m_activeRecordingInputSource`.

- [ ] Extend the recording-page UI test: source combo has array/noise modes, array mode shows 48-channel panel, noise mode shows four checkable stage sources, clearing all stages disables start, and layout has combined/separate choices.
- [ ] Extend the minimum-size test to visit both source modes at 1366×768 and assert every visible source control stays inside the recording page without overlap.
- [ ] Run focused UI tests and verify RED on missing controls.
- [ ] Build the stacked source UI and layout combo, keeping current array controls and default selected output channel.
- [ ] Update start validation and configuration locking to include source, stage, and layout controls.
- [ ] Route adjusted frames only for array sessions and noise frames only for noise sessions.
- [ ] Rebuild and run focused UI tests; verify GREEN.

### Task 6: Noise-Page Shortcut and Background Processing

**Files:**

- Modify: `src/ui/noisereductionpage.h`
- Modify: `src/ui/noisereductionpage.cpp`
- Modify: `src/ui/mainwindow.h`
- Modify: `src/ui/mainwindow.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**

- Add `NoiseReductionPage::setRecordingRequired(bool, const QVector<NoiseReductionDataFlowStage> &)`.
- Add `NoiseReductionPage::setQuickRecordingState(bool active, bool canStart, const QString &message)`.
- Add signal `quickRecordingToggleRequested()` and forwarded `noiseRecordingFrameReady(...)`.
- Worker-active condition becomes `(m_active || m_recordingRequired) && m_settings.enabled`.

- [ ] Add a page test proving `submitRawAudioFrame()` still increments submitted frames when page inactive but recording required, and stops doing so after the requirement is cleared.
- [ ] Add a MainWindow test that the quick button shares the same start/stop buttons' state transition and retains recording-page source/layout selections; mutation caught: a separate recorder or hard-coded shortcut configuration.
- [ ] Add a test that a noise session keeps capture active after switching to the recording tab and releases it on Idle/Error.
- [ ] Run focused UI tests and verify RED on missing API/button.
- [ ] Add the shortcut to the monitor tab, forward typed recording frames, and centralize worker-active emission.
- [ ] Connect MainWindow session state to background requirement and quick-button text/enabled state; synchronize errors to the noise status label.
- [ ] Rebuild and run focused and full UI tests; verify GREEN.

### Task 7: Documentation, Review, and Full Verification

**Files:**

- Modify: `doc/Readme.md`
- Update: `docs/superpowers/specs/2026-08-02-noise-monitor-buffer-and-multisource-recording-design.md`
- Update: `docs/superpowers/plans/2026-08-02-noise-monitor-buffer-and-multisource-recording.md`

- [x] Document monitor latency/smoothness trade-off, overflow behavior, four stage semantics, source exclusivity, both layouts, filenames, shortcut behavior, background processing, and failure policy.
- [x] Add a 2026-08-02 change-record row.
- [x] Run Debug full build and all six CTest targets with the Qt 6.11.1 MinGW runtime on PATH.
- [x] Run `python -m unittest discover -s tests/tools -p "test_*.py" -v` and `python tools/mock_device_server.py --self-test`.
- [x] Request a read-only independent review focused on audio continuity, bounded queues, track alignment, partial writer failure, thread transitions, shortcut reuse, and preservation of existing recording behavior; fix every Critical/Important finding. （controller 最终审查已完成且无 Critical/Important；本 fix wave 随后由 controller scoped re-review。）
- [x] Run fresh full verification, `git diff --check`, `git status --short`, and leave all changes unstaged and uncommitted.
