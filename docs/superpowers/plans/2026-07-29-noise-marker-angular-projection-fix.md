# Noise Marker Angular Projection Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make noise-reduction video marker positions and clicked/dragged angles accurate across a 180-degree field of view by using an exact angle-linear wide-angle projection.

**Architecture:** Add a separate angular projection pair beside the existing rectilinear camera functions, sharing the same camera orientation and roll conventions but allowing FOV values through 180 degrees. Route only `NoiseReductionPage` and `NoiseReductionVideoWidget` through the new pair so photo calibration and live-localization projection semantics remain unchanged.

**Tech Stack:** C++20, Qt 6 Core/Gui/Widgets/Test, existing direction-vector and camera-view infrastructure.

## Global Constraints

- Preserve all existing uncommitted work.
- Work directly in the current `master` checkout; do not create a worktree, branch, or commit.
- Keep existing `projectDirection()` and `unprojectVideoPoint()` rectilinear behavior unchanged.
- Do not change control-code quantization, persistent configuration format, or release-only direction sending.
- Accept angular projection fields of view only when `1° < FOV ≤ 180°`.

---

### Task 1: Define Angular Projection Behavior with Failing Tests

**Files:**
- Modify: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Specifies the desired interfaces:

```cpp
ProjectionResult projectDirectionAngular(
    const SoundSourceDirection &direction,
    const CameraViewSettings &camera);

ScanDirection unprojectVideoPointAngular(
    const QPointF &normalizedPoint,
    const CameraViewSettings &camera);
```

- [ ] **Step 1: Add wide-angle boundary test**

Add `projectsDirectionsLinearlyAcrossWideAngleView()` with a `180° × 180°` camera. Assert literal positions:

```cpp
QCOMPARE(projectDirectionAngular({-90.0, 0.0}, camera).normalizedPoint,
         QPointF(0.0, 0.5));
QCOMPARE(projectDirectionAngular({0.0, 0.0}, camera).normalizedPoint,
         QPointF(0.5, 0.5));
QCOMPARE(projectDirectionAngular({90.0, 0.0}, camera).normalizedPoint,
         QPointF(1.0, 0.5));
QCOMPARE(projectDirectionAngular({45.0, 0.0}, camera).normalizedPoint,
         QPointF(0.75, 0.5));
```

Use floating tolerances instead of exact comparison where vector conversion rounding requires it, and assert all four results are valid, in front, and in view.

- [ ] **Step 2: Add oriented round-trip test**

Add `roundTripsAngularProjectionWithOrientationAndRoll()`. Configure non-zero center azimuth, center elevation and roll, project several known in-view directions, unproject their normalized positions, and assert azimuth/elevation errors below `1.0e-5°`.

- [ ] **Step 3: Build and verify RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
```

Expected: compilation fails because the two angular projection functions do not exist.

---

### Task 2: Implement the Angle-Linear Projection Pair

**Files:**
- Modify: `src/analysis/directionprojection.h`
- Modify: `src/analysis/directionprojection.cpp`
- Test: `tests/analysis/test_audioanalysis.cpp`

**Interfaces:**
- Consumes: `directionFromAngles()`, `anglesFromDirection()`, camera center angles, roll, and FOV.
- Produces: the two functions specified in Task 1 without changing existing rectilinear functions.

- [ ] **Step 1: Declare the two angular projection interfaces**

Add the exact signatures specified by Task 1 to `directionprojection.h`.

- [ ] **Step 2: Add angular-camera validation**

Validate finite center angles and roll using their existing ranges, and validate both FOV values with:

```cpp
fov > 1.0 && fov <= 180.0
```

Return the same NaN direction contract as the existing inverse projection for invalid values.

- [ ] **Step 3: Build the rolled camera basis**

Use the same forward/right/up and roll equations as the existing projection. Factor this repeated calculation into a private helper only if both old and new functions retain identical behavior.

- [ ] **Step 4: Implement forward angular projection**

Calculate local right/up/forward components, then:

```cpp
const double horizontalDegrees =
    qRadiansToDegrees(std::atan2(xc, zc));
const double verticalDegrees =
    qRadiansToDegrees(std::atan2(yc, std::hypot(xc, zc)));
```

Map these angles linearly into normalized coordinates. Treat `|horizontalDegrees| ≤ 90°` as in front so exact `±90°` edges remain visible. Reuse edge intersection behavior for finite out-of-view points.

- [ ] **Step 5: Implement inverse angular projection**

Convert normalized coordinates into horizontal and vertical radians, construct:

```cpp
forward * (cosVertical * cosHorizontal)
    + rolledRight * (cosVertical * sinHorizontal)
    + rolledUp * sinVertical
```

Normalize and convert back to azimuth/elevation.

- [ ] **Step 6: Run focused analysis tests and verify GREEN**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_analysis
build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug\test_analysis.exe projectsDirectionsLinearlyAcrossWideAngleView roundTripsAngularProjectionWithOrientationAndRoll
```

Expected: both new tests pass, and existing rectilinear projection tests remain unchanged.

---

### Task 3: Route Noise-Reduction Markers Through Angular Projection

**Files:**
- Modify: `tests/ui/test_dashboard.cpp`
- Modify: `src/ui/noisereductionpage.cpp`
- Modify: `src/ui/noisereductionvideowidget.cpp`

**Interfaces:**
- Consumes: the Task 1 angular projection functions.
- Produces: accurate click, drag and marker rendering for normal and presentation views.

- [ ] **Step 1: Add failing UI click expectation**

Change the existing click regression to click at 75% of the rendered video width under the default `180°` view. Assert the selected target is approximately `+45°` azimuth and `0°` elevation, using a tolerance derived only from one displayed pixel.

- [ ] **Step 2: Add marker round-trip assertion**

After a drag release, use the page's resulting angle and the expected hand-derived angular mapping to verify that the marker corresponds to the released normalized position. Keep the existing assertions that no control send occurs during drag and exactly one occurs on release.

- [ ] **Step 3: Run focused UI tests and verify RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug\test_ui.exe noiseReductionVideoClickUpdatesSelectedTargetAndRequestsAllControls noiseReductionPageSendsDraggedMarkerOnlyOnRelease
```

Expected: the old `179°` rectilinear path returns an angle near `+90°`, so the new linear-angle assertions fail.

- [ ] **Step 4: Switch marker painting to angular projection**

In `NoiseReductionVideoWidget`, retain the configured camera value without clamping it to `179°`, and use `projectDirectionAngular()` in `targetPoint()`.

- [ ] **Step 5: Switch clicks and drags to angular inverse projection**

In `NoiseReductionPage`, replace both `unprojectVideoPoint()` calls with `unprojectVideoPointAngular()`. Remove `projectionSafeCamera()` and `snapVideoCenter()` so no hidden FOV clamp or center dead zone remains.

- [ ] **Step 6: Run focused UI tests and verify GREEN**

Run the Task 3 focused commands and expect both click and drag tests to pass.

---

### Task 4: Documentation and Complete Verification

**Files:**
- Modify: `doc/Readme.md`
- Verify: all current workspace changes

**Interfaces:**
- Produces: maintained behavior documentation and fresh verification evidence.

- [ ] **Step 1: Update documentation**

Document that the noise-reduction video uses angle-linear wide-angle projection, that `180°` edges map to `±90°`, and that other camera calibration paths retain rectilinear projection. Add a `2026-07-29` change-record entry.

- [ ] **Step 2: Build and run all C++ tests**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
```

- [ ] **Step 3: Run tools and diff checks**

Run:

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
git diff --check
git status --short
```

Expected: all tests and self-test pass, no whitespace errors are reported, and only intentional uncommitted changes remain.
