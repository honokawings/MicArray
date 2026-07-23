# CPU SRP-PHAT Heatmap Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the real `640×360` SRP-PHAT heatmap sustain at least 10 FPS in a Release build on the current development machine.

**Architecture:** Collapse 122 microphone-pair correlations into four equal-baseline PHAT groups, reuse grouped correlations between localization and heatmap evaluation, and precompute grid steering indices. Scan the cached grid in contiguous row chunks on a dedicated Qt thread pool while preserving the existing independent live-localization worker boundary.

**Tech Stack:** C++20, Qt 6 Core/Concurrent, Qt Test, CMake, existing radix-2 FFT.

## Global Constraints

- Pure CPU only; no GPU backend or third-party runtime.
- Evaluate every direction in the requested grid; no coarse-to-fine interpolation.
- `640×360` Release median after warm-up must be at most 100 ms.
- Leave one logical CPU available for the primary media/UI paths.
- Do not create a branch, stage files, or commit.

---

### Task 1: Establish correctness and performance baselines

**Files:**
- Modify: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Consumes: `ISpatialResponseProvider::evaluateSpatialResponse(...)`.
- Produces: regression tests for maximum-grid output and elapsed time.

- [ ] Add `evaluatesMaximumSpatialGridWithinCpuBudget()` that builds a `640×360` field-of-view grid, evaluates a synthetic plane wave, verifies 230,400 finite values and reports elapsed milliseconds.
- [ ] Add `spatialResponseMatchesAfterLocalizationWarmup()` using two locators to compare direct evaluation with `locate()` followed by evaluation on the same window.
- [ ] Build and run the two tests against the current implementation.
- [ ] Record the current Debug elapsed time and confirm the 100 ms maximum-grid assertion fails for the performance reason.
- [ ] Configure `build/perf` as Release, build `test_analysis`, run the same maximum-grid test, and record the unoptimized Release elapsed time.

### Task 2: Add grid preparation and grouped PHAT correlations

**Files:**
- Modify: `src/analysis/ispatialresponseprovider.h`
- Modify: `src/analysis/gccphatlocator.h`
- Modify: `src/analysis/gccphatlocator.cpp`
- Modify: `src/analysis/livelocalizationworker.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Produces: `ISpatialResponseProvider::prepareSpatialResponseGrid(const SpatialResponseGrid &)`.
- Produces: cached steering arrays and four grouped correlation buffers inside `GccPhatLocator`.
- Consumes: prepared grid in `evaluateSpatialResponse(...)`.

- [ ] Add a failing test that prepares one grid, evaluates it, prepares a different same-size grid, and verifies the peak follows the new directions.
- [ ] Add the default no-op preparation method to the extension interface and invoke it from `LiveLocalizationWorker::setSpatialGrid()`.
- [ ] Group pairs by quantized `(dxMeters, dyMeters)`, validate that the default geometry produces four groups, and invalidate all caches on `configure()`.
- [ ] Refactor spectrum generation into a helper shared by direct heatmap evaluation and localization.
- [ ] Accumulate normalized PHAT cross spectra per group and perform four inverse FFTs.
- [ ] During `locate()`, populate the grouped-correlation window cache while retaining the existing individual-pair TDOA path.
- [ ] Precompute wrapped correlation indices and interpolation fractions for every grid direction and baseline group.
- [ ] Replace the 122-pair direction loop with four contiguous cached interpolations and standard SRP-PHAT summation.
- [ ] Run analysis correctness tests and confirm peak, invalid-input, warm-cache and grid-invalidation behavior.

### Task 3: Parallelize the maximum-grid scan

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/analysis/gccphatlocator.h`
- Modify: `src/analysis/gccphatlocator.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Consumes: cached group correlations and steering arrays from Task 2.
- Produces: dedicated `QThreadPool` row-chunk scanning through Qt Concurrent.

- [ ] Link `Qt6::Concurrent` for the application and shared test library.
- [ ] Configure the locator pool for `max(1, QThread::idealThreadCount() - 1)` workers.
- [ ] Partition directions into contiguous row-aligned chunks and write disjoint output spans.
- [ ] Keep normalization and reliability reduction deterministic after all chunks complete.
- [ ] Run the Debug maximum-grid test and record elapsed time.
- [ ] Rebuild `build/perf` in Release and verify the warm median is at most 100 ms.
- [ ] If the target is missed, profile the grouped scan and optimize only the measured remaining hotspot before repeating the benchmark.

### Task 4: Documentation and full regression

**Files:**
- Modify: `doc/Readme.md`

- [ ] Document four-baseline PHAT aggregation, window/steering caches, CPU thread usage, and the measured before/after results.
- [ ] Run the full Debug build and all six C++ tests.
- [ ] Run all Python tests and the mock-device self-test.
- [ ] Run `git diff --check` and inspect `git status --short --branch`.
- [ ] Leave all changes unstaged and uncommitted on `master`.

