# Configurable Live Heatmap Grid Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add persistent, independently adjustable live heatmap grid dimensions up to `640×360`.

**Architecture:** Store grid dimensions in `LiveLocalizationSettings`, validate them in `AppSettings`, and bind two `QSpinBox` controls in `LiveLocalizationPage`. `rebuildSpatialGrid()` consumes the stored dimensions without changing the worker or spatial-response extension interface.

**Tech Stack:** C++20, Qt 6 Widgets, QSettings, Qt Test, CMake.

## Global Constraints

- Grid columns default to `64` and must remain in `16..640`.
- Grid rows default to `36` and must remain in `9..360`.
- Changes rebuild the grid immediately and persist locally.
- Do not create a branch, stage files, or commit.

---

### Task 1: Persist and validate grid dimensions

**Files:**
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Test: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `LiveLocalizationSettings::heatmapGridColumns`
- Produces: `LiveLocalizationSettings::heatmapGridRows`

- [ ] Add failing persistence assertions using `320×180`.
- [ ] Add failing invalid-field assertions for values outside `16..640` and `9..360`.
- [ ] Run `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_app && ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_app --output-on-failure` and confirm failure.
- [ ] Add the two fields, QSettings read/write keys, and independent sanitization.
- [ ] Run the same test command and confirm it passes.

### Task 2: Add page controls and rebuild behavior

**Files:**
- Modify: `src/ui/livelocalizationpage.h`
- Modify: `src/ui/livelocalizationpage.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces: widgets `liveHeatmapGridColumnsSpin` and `liveHeatmapGridRowsSpin`.
- Consumes: persisted grid dimensions from Task 1.

- [ ] Add failing UI assertions for saved values and maximum limits.
- [ ] Run `cmake --build build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui && ctest --test-dir build/Desktop_Qt_6_11_1_MinGW_64_bit_Debug -R test_ui --output-on-failure` and confirm failure.
- [ ] Add the spin boxes, restore their values, persist changes, and generate exactly `columns × rows` directions.
- [ ] Run the same UI test command and confirm it passes.

### Task 3: Documentation and full verification

**Files:**
- Modify: `doc/Readme.md`

- [ ] Document the adjustable range, default, persistence, and high-resolution performance trade-off.
- [ ] Run the complete Debug build.
- [ ] Run all six C++ tests, all Python tests, the mock-device self-test, and `git diff --check`.
- [ ] Confirm `git status --short --branch` remains on `master` with only unstaged/untracked changes.

