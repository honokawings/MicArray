# Trusted Heatmap and Neural Halo Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make realtime heatmap semantics match trusted-source detection and remove the neural 3σ ring artifact without weakening physical detection gates.

**Architecture:** `LiveLocalizationWorker` publishes an explicit same-window trusted flag and an informative marginal-candidate status. `LiveLocalizationView` only mutes untrusted visualization, while `sharpenNeuralSrpHeatmap()` blends the enhanced core back to the untouched physical map across 2σ..3σ.

**Tech Stack:** C++20, Qt 6 Core/Widgets/Test, CMake.

## Global Constraints

- Do not change the default confidence threshold or physical reliability calculation.
- Keep values outside neural 3σ bit-for-bit unchanged.
- Do not change camera projection, calibration, grid resolution, or worker threading.
- Preserve all existing workspace edits; do not branch, stage, or commit.
- Update `doc/Readme.md` and its change record.

---

## File Map

- Modify `src/analysis/localizationtypes.h`: add the explicit live trusted flag.
- Modify `src/analysis/livelocalizationworker.cpp`: populate the flag and report marginal reliability/threshold.
- Modify `src/ui/livelocalizationview.cpp`: render untrusted candidates with a fixed cold-color scale.
- Modify `src/analysis/robustsrpphatscorer.cpp`: smooth the neural enhancement boundary.
- Modify `tests/analysis/test_audioanalysis.cpp`: worker contract and numerical continuity regressions.
- Modify `tests/ui/test_dashboard.cpp`: trusted/untrusted rendering regression.
- Modify `doc/Readme.md`: behavior and change record.

### Task 1: Lock Down the Regressions

**Files:**

- Test: `tests/analysis/test_audioanalysis.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**

- Produces expected field `LiveSpatialResponse::trustedDirection`.
- Exercises existing `sharpenNeuralSrpHeatmap(...)` and `LiveLocalizationView::setSpatialResponse(...)`.

- [x] Add `neuralPeakHeatmapBlendsContinuouslyAtThreeSigma()` using a one-row radial grid with a high original shoulder; assert the 3σ sample equals the original, the just-inside jump is bounded, outside samples are unchanged, and the core remains stronger than its shoulder.
- [x] Add `liveWorkerMarksMarginalSpatialPeakUntrusted()` with `confidenceThreshold = 1.0`; feed a deterministic plane wave and assert a non-empty response, an empty direction result, `trustedDirection == false`, and a status containing both “候选可靠度” and “门限”.
- [x] Add `liveHeatmapMutesUntrustedRelativePeak()`; render identical response values with false/true trusted flags and assert the untrusted peak hue remains cold while the trusted peak is red.
- [x] Build `test_analysis` and `test_ui`, then run their focused tests. Expected RED: the trusted field is missing and the neural boundary continuity assertion fails.

### Task 2: Implement the Trusted Heatmap Contract

**Files:**

- Modify: `src/analysis/localizationtypes.h`
- Modify: `src/analysis/livelocalizationworker.cpp`
- Modify: `src/ui/livelocalizationview.cpp`

**Interfaces:**

- Produces `bool LiveSpatialResponse::trustedDirection = false`.
- Consumes `directions.isEmpty()`, `spatial.reliability`, and `m_settings.confidenceThreshold` from the same analysis call.

- [x] Add the default-false field to `LiveSpatialResponse`.
- [x] Set it to `!directions.isEmpty()` before emitting every non-empty spatial response.
- [x] For an untrusted valid response, emit `候选可靠度 %1%，未达到门限 %2%`; for a trusted response retain `实时声源定位正常`.
- [x] In `colorForValue`, apply the existing focus exponent first, then multiply by `0.35` only when the response is untrusted so every allowed exponent keeps the peak cold, and finally use the HSV mapping.
- [x] Run the worker/UI focused tests and confirm GREEN.

### Task 3: Remove the Neural 3σ Discontinuity

**Files:**

- Modify: `src/analysis/robustsrpphatscorer.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**

- Keeps `sharpenNeuralSrpHeatmap(...)` signature unchanged.

- [x] Preserve the original cell value before calculating shoulder/kernel.
- [x] Calculate `edgeBlend = smoothStep(4.0, 9.0, mahalanobisSquared)` and store `enhanced * (1 - edgeBlend) + original * edgeBlend` for cells inside 3σ.
- [x] Keep the current early exits, bounds checks, and exact skip path outside 3σ.
- [x] Run the numerical continuity test and the existing main-lobe contraction test; confirm both GREEN.

### Task 4: Documentation and Full Verification

**Files:**

- Modify: `doc/Readme.md`

- [x] Document the relative-response versus trusted-source distinction, cold display for untrusted candidates, reliability/threshold status, and smooth 2σ..3σ neural transition.
- [x] Add a 2026-07-31 change-record entry.
- [x] Run Debug build and full CTest.
- [x] Run Python tool tests and mock-device self-test.
- [x] Run `git diff --check`, inspect `git status --short`, and verify no existing edits were discarded.
- [x] Leave all changes unstaged and uncommitted.
