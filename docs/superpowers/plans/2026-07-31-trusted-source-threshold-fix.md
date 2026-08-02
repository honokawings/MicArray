# Trusted Source Threshold Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the realtime trusted-source threshold the single effective confidence gate, reduce the realtime default to `0.40`, and preserve false-positive rejection.

**Architecture:** `RobustSrpPhatScorer` will report every mathematically valid continuous candidate and keep channel coherence as a continuous reliability factor. `GccPhatLocator` remains the sole trusted/untrusted gate through `confidenceThreshold`; realtime settings gain a migration-aware preferred key and clearer UI while offline/core defaults stay unchanged.

**Tech Stack:** C++20, Qt 6 Core/Widgets/Test, CMake, QSettings.

## Global Constraints

- Do not change the SRP direction, heatmap values, reliability five-factor formula, neural model, projection, calibration, grid, or threading.
- Realtime default trust threshold is `0.40`; offline and core `LocatorConfiguration` defaults remain `0.45`.
- Structural invalidity, silence, insufficient channels/groups, invalid Hessian, diffuse noise, and narrowband ambiguity remain rejected.
- Keep the existing `liveLocalizationConfidenceSpin` object name and generation behavior.
- Preserve all current workspace edits; do not create a branch, stage, or commit.
- Update `doc/Readme.md` and its change record.

---

## File Map

- Modify `src/analysis/robustsrpphatscorer.cpp`: separate candidate validity from trusted threshold.
- Modify `src/app/appsettings.h/.cpp`: realtime default and migration-aware preferred key.
- Modify `src/ui/livelocalizationpage.h/.cpp`: clear label, step, tooltip, and wheel protection.
- Modify `tests/analysis/test_audioanalysis.cpp`: low-coherence accurate-source and false-positive gates.
- Modify `tests/app/test_app.cpp`: new/legacy/invalid persistence behavior.
- Modify `tests/ui/test_dashboard.cpp`: control contract and generation behavior.
- Modify `doc/Readme.md`: algorithm semantics, tuning guidance, migration, and change record.

### Task 1: Prove and Remove the Hidden Hard Gate

**Files:**

- Modify: `tests/analysis/test_audioanalysis.cpp`
- Modify: `src/analysis/robustsrpphatscorer.cpp`

**Interfaces:**

- Consumes `RobustSpatialEstimate::{hasDirection,reliability,steeredChannelCoherence,direction}`.
- Keeps `RobustSrpPhatScorer::estimate(...)` and `GccPhatLocator::locate(...)` signatures unchanged.

- [ ] Add a deterministic test helper that starts from `syntheticPlaneWave(18.35, -6.75)` and adds independently seeded Gaussian noise to each channel at `-15 dB` SNR using seed `0x54525553`. The fixture must produce an angular error below `2°`, `steeredChannelCoherence < 0.15`, finite positive Hessian widths, and reliability strictly between `0.30` and `0.60` on two consecutive runs.
- [ ] Add `lowCoherenceAccuratePeakObeysConfiguredTrustThreshold()`. Configure two real `GccPhatLocator` instances with thresholds `0.30` and `0.60`; assert the low-threshold locator returns one accurate direction and the high-threshold locator returns none. The production mutation this catches is restoring a hidden scorer coherence gate or ignoring the configured threshold.
- [ ] Run the focused test and verify RED: both locators return no direction because current `hasDirection` contains the fixed `>= 0.15` condition. Include diagnostic values in the failure message to tune only the fixture SNR, not the expected behavior.
- [ ] Remove only `result->steeredChannelCoherence >= 0.15` from the `hasDirection` expression. Keep positive reliability and positive azimuth/elevation standard deviations.
- [ ] Run the focused test and existing accurate/noisy/corrupt-channel analysis tests; verify GREEN.

### Task 2: Preserve Physical False-Positive Safety

**Files:**

- Modify: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**

- Exercises real `GccPhatLocator` with `confidenceThreshold = 0.40`.

- [ ] Add `realtimeDefaultThresholdRejectsAmbiguousEvidence()`: feed silence, the eight existing independently seeded diffuse-noise windows, and `350/1000/3500/7000 Hz` narrowband plane waves into fresh or explicitly reset real locators; assert every `locate()` result is empty. The production mutations this catches are a too-low default recommendation or bypassing structural evidence gates.
- [ ] Add an explicit threshold-zero assertion to the existing insufficient-channel test: even at `0.0`, 31 valid channels cannot produce a direction.
- [ ] Run the new tests before changing defaults. The measured diffuse counterexample reaches `0.366`, so use the literal realtime default `0.40`; verify it rejects all diffuse/narrowband fixtures while the accurate low-coherence fixture remains accepted below its measured `0.490` reliability.
- [ ] Run full `test_analysis`; verify all precision, neural safety, CPU, diffuse, narrowband, and worker tests pass.

### Task 3: Add Migration-Aware Realtime Threshold Persistence

**Files:**

- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Modify: `tests/app/test_app.cpp`

**Interfaces:**

- `LiveLocalizationSettings::confidenceThreshold` default becomes `0.40`.
- Preferred key: `liveLocalization/trustedSourceThreshold`.
- Legacy compatibility key: `liveLocalization/confidenceThreshold`.

- [ ] Add failing tests for five observable behaviors: a new settings file returns `0.40`; preferred-key round trip preserves a non-default `0.34`; legacy-only `0.45` migrates to `0.40`; legacy-only custom `0.38` is preserved; invalid preferred values (`NaN`, `-0.01`, `1.01`) independently fall back to `0.40` even if the legacy key is valid.
- [ ] Run `test_app` and verify RED on the new default/preferred-key/migration expectations.
- [ ] Change only the live settings default to `0.40`. In `AppSettings::liveLocalization()`, prefer `trustedSourceThreshold`; when absent, read the legacy key, map exact old default `0.45` to the new default, and preserve other valid values. Pass the result through existing finite `[0,1]` sanitization.
- [ ] In `saveLiveLocalization()`, write the sanitized value to both the preferred and legacy keys.
- [ ] Run focused and full `test_app`; verify GREEN and confirm audio-analysis `0.45` tests remain unchanged.

### Task 4: Clarify and Protect the Realtime Control

**Files:**

- Modify: `src/ui/livelocalizationpage.h`
- Modify: `src/ui/livelocalizationpage.cpp`
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**

- Keep object name `liveLocalizationConfidenceSpin`.
- Add `LiveLocalizationPage::eventFilter(QObject *, QEvent *)` for this one protected control.

- [ ] Extend the real-page UI test to assert threshold value `0.40`, range `[0,1]`, two decimals, step `0.02`, a tooltip containing the lower-threshold/false-positive tradeoff, and a visible form label “可信判定阈值”.
- [ ] Add `liveLocalizationTrustThresholdIgnoresMouseWheel()`: focus the real spin box, send a `QWheelEvent`, and assert the value is unchanged. Removing the event filter must fail this test.
- [ ] Extend the generation test: changing threshold by `0.02` emits exactly one `configureWorkerRequested`, exactly one new spatial generation, and persists the preferred-key value.
- [ ] Run focused UI tests and verify RED on default/step/label/wheel expectations.
- [ ] Configure the spin box with two decimals, step `0.02`, explanatory tooltip and form label “可信判定阈值”; install an event filter that accepts wheel events only for this spin box.
- [ ] Run focused and full `test_ui`; verify GREEN.

### Task 5: Documentation, Review, and Full Verification

**Files:**

- Modify: `doc/Readme.md`
- Update: `docs/superpowers/specs/2026-07-31-trusted-source-threshold-fix-design.md`
- Update: `docs/superpowers/plans/2026-07-31-trusted-source-threshold-fix.md`

- [ ] Document candidate validity versus trusted gating, removal of the hidden `0.15` hard gate, realtime default/migration, tuning tradeoff, and unchanged offline default.
- [ ] Add a 2026-07-31 change-record row with exact verification commands.
- [ ] Run Debug full build and all 6 CTest targets.
- [ ] Build `train_neural_srp_peak` and run `--self-test`.
- [ ] Run all 45 Python tool tests and mock-device self-test.
- [ ] Request a read-only independent review focused on false positives, migration, generation, cross-thread settings, and the removal of the redundant gate; fix every Critical or Important issue.
- [ ] Run `git diff --check`, inspect `git status --short`, and leave every change unstaged and uncommitted.
