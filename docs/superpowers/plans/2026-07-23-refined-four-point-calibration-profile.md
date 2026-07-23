# Refined Four-Point Calibration Profile Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refine four-point video calibration with sample inspection/undo, hideable completed overlays, and validated JSON profile import/export.

**Architecture:** Add a UI-independent calibration profile store for atomic JSON persistence, extend application settings for overlay visibility and reference video size, then bind those capabilities into `LiveLocalizationPage`. The view remains presentation-only and receives either working samples, saved samples, or an empty overlay.

**Tech Stack:** C++20, Qt 6 Core/Widgets, QJsonDocument, QSaveFile, Qt Test, CMake.

## Global Constraints

- JSON format identifier is `MicArrayVideoCalibration`, version `1`.
- Sample count is exactly `0` or `4`.
- Failed load must not mutate current calibration state.
- Working samples remain visible during calibration.
- Successful calibration automatically hides completed calibration points.
- Do not create a branch, stage files, or commit.

---

### Task 1: Calibration profile JSON store

**Files:**
- Create: `src/analysis/calibrationprofile.h`
- Create: `src/analysis/calibrationprofile.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Produces: `VideoCalibrationProfile`.
- Produces: `VideoCalibrationProfileStore::save(path, profile, error)`.
- Produces: `VideoCalibrationProfileStore::load(path, profile, error)`.

- [ ] Add failing tests for complete-profile round-trip, camera-only round-trip, and malformed/unsupported/invalid-sample rejection.
- [ ] Run `test_analysis` and confirm compilation fails because the store does not exist.
- [ ] Implement strict finite/range validation, JSON encoding/decoding, ISO UTC timestamp and atomic `QSaveFile` writes.
- [ ] Add the new source/header to CMake.
- [ ] Run `test_analysis` and confirm all analysis tests pass.

### Task 2: Persist overlay visibility and reference video size

**Files:**
- Modify: `src/app/appsettings.h`
- Modify: `src/app/appsettings.cpp`
- Test: `tests/app/test_app.cpp`

**Interfaces:**
- Produces: `LiveLocalizationSettings::showCalibrationOverlay`.
- Produces: `LiveLocalizationSettings::calibrationReferenceVideoSize`.

- [ ] Add failing persistence assertions for visibility and `1920×1080` reference size.
- [ ] Add invalid negative/oversized dimension assertions that independently fall back to `0×0`.
- [ ] Run `test_app` and confirm failure because fields are absent.
- [ ] Add settings fields, sanitization and QSettings read/write keys.
- [ ] Run `test_app` and confirm pass.

### Task 3: Refined calibration page workflow

**Files:**
- Modify: `src/ui/livelocalizationpage.h`
- Modify: `src/ui/livelocalizationpage.cpp`
- Test: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Produces widgets `undoCalibrationPointButton`, `showCalibrationOverlayCheck`, `calibrationSamplesTable`, `exportVideoCalibrationButton`, and `importVideoCalibrationButton`.
- Consumes `VideoCalibrationProfileStore` and Task 2 settings.

- [ ] Add failing UI tests for the five new controls, saved visibility, and four-row detail table.
- [ ] Add a failing workflow test that starts calibration, accepts two samples, undoes one, and confirms the table/progress return to one sample.
- [ ] Add a failing application test helper path that imports a profile without a file dialog and verifies camera/sample/settings updates atomically.
- [ ] Build and run `test_ui`, confirming missing controls/API failures.
- [ ] Build the controls and connect undo/show/export/import actions.
- [ ] Add `refreshCalibrationDetails()` and update it after start, accept, undo, cancel, finish, clear and import.
- [ ] Filter completed overlay samples through `showCalibrationOverlay`; always render working samples.
- [ ] On successful fit store reference video size, set visibility false, persist, and refresh.
- [ ] Implement profile export/import application with file dialogs and a directly testable `loadCalibrationProfile(path)` method.
- [ ] Run `test_ui` and confirm all UI tests pass.

### Task 4: Documentation and verification

**Files:**
- Modify: `doc/Readme.md`

- [ ] Document undo, sample details, overlay visibility and JSON schema behavior.
- [ ] Run the full Debug build and all six C++ tests.
- [ ] Run all Python tests and mock-device self-test.
- [ ] Run `git diff --check` and inspect `git status --short --branch`.
- [ ] Leave all changes unstaged and uncommitted on `master`.

