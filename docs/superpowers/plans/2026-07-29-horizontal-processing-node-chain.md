# Horizontal Processing Node Chain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the four noise-reduction data-flow nodes into a single horizontal chain that always shows every node without scrolling while preserving selection and constrained reordering.

**Architecture:** Keep `ProcessingNodeListWidget` as the node-order owner and switch its `QListView` geometry to a non-wrapping left-to-right flow. Recompute equal card widths whenever the viewport is resized, keep compact state text on each card, and move detailed parameter summaries into tooltips.

**Tech Stack:** C++20, Qt 6 Widgets, Qt Test, existing `ProcessingNodeListWidget` and `NoiseReductionPage`.

## Global Constraints

- Preserve all pre-existing uncommitted changes.
- Work directly in the current `master` checkout; do not create a worktree, branch, or commit.
- Show all four nodes simultaneously without horizontal or vertical scrolling.
- Keep suppression fixed first and keep the remaining three nodes reorderable.
- Do not change DSP, persistence, or node parameter behavior.

---

### Task 1: Specify the Horizontal Chain with a Failing UI Test

**Files:**
- Modify: `tests/ui/test_dashboard.cpp`

**Interfaces:**
- Consumes: `ProcessingNodeListWidget` through its existing public Qt widget API.
- Produces: regression coverage for flow, wrapping, scroll policy, card geometry, and drag flags.

- [ ] **Step 1: Extend `noiseReductionPageUsesTabbedReorderableDataFlow`**

After locating the real node widget, assert:

```cpp
QCOMPARE(nodes->flow(), QListView::LeftToRight);
QVERIFY(!nodes->isWrapping());
QCOMPARE(nodes->horizontalScrollBarPolicy(),
         Qt::ScrollBarAlwaysOff);
QCOMPARE(nodes->verticalScrollBarPolicy(),
         Qt::ScrollBarAlwaysOff);
```

After processing pending layout events, assert that each of the four `visualItemRect()` values is non-empty and contained by `nodes->viewport()->rect()`. Retain the existing assertion that item 0 is not draggable, and add assertions that items 1–3 are draggable.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --target test_ui
build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug\test_ui.exe noiseReductionPageUsesTabbedReorderableDataFlow
```

Expected: the test fails because the current widget uses the default top-to-bottom flow and scrolling policies.

---

### Task 2: Implement the Responsive Horizontal Node Chain

**Files:**
- Modify: `src/ui/processingnodelistwidget.h`
- Modify: `src/ui/processingnodelistwidget.cpp`

**Interfaces:**
- Consumes: widget viewport width, item count, node state and parameter summary.
- Produces: `void resizeEvent(QResizeEvent *event) override`, `updateItemSizes()`, and compact horizontal node cards.

- [ ] **Step 1: Configure horizontal single-row behavior**

In the constructor configure:

```cpp
setFlow(QListView::LeftToRight);
setWrapping(false);
setResizeMode(QListView::Adjust);
setUniformItemSizes(true);
setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
```

Replace the tall list minimum with a compact fixed height sufficient for the two-line card.

- [ ] **Step 2: Add responsive card sizing**

Override `resizeEvent`, invoke the base implementation, then call `updateItemSizes()`. Compute a conservative equal width from:

```cpp
const int available =
    viewport()->width() - spacing() * (count() + 1);
const int itemWidth = std::max(1, available / count());
```

Set every item to the same two-line card height and calculated width. Also call `updateItemSizes()` after rebuilding or restoring the node order.

- [ ] **Step 3: Compact labels and preserve details**

Store enabled state and summary in item data roles. Render only the drag/fixed symbol, node name, state, and a right arrow on every card except the final card. Put the full parameter summary in `QListWidgetItem::toolTip()` so it cannot affect layout.

- [ ] **Step 4: Keep arrows correct after reordering**

Refresh all node texts after `dropEvent()` and `restoreFixedFirstNode()` so the final card never displays an outgoing arrow and every preceding card does.

- [ ] **Step 5: Run the focused test and verify GREEN**

Run the Task 1 build and focused-test commands. Expected: the horizontal geometry and existing node-order assertions pass.

---

### Task 3: Documentation and Full Verification

**Files:**
- Modify: `doc/Readme.md`
- Verify: all current workspace changes

**Interfaces:**
- Produces: maintained UI behavior documentation and fresh verification evidence.

- [ ] **Step 1: Update the data-flow documentation**

Describe the single-row four-node chain, no-scroll width adaptation, fixed suppression node, and horizontal drag ordering. Add a `2026-07-29` change-record entry.

- [ ] **Step 2: Build and run all C++ tests**

Run:

```powershell
cmake --build build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug
ctest --test-dir build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug --output-on-failure
```

Expected: complete build succeeds and every configured C++ test passes.

- [ ] **Step 3: Run tool verification**

Run:

```powershell
python -m unittest discover -s tests/tools -p "test_*.py" -v
python tools/mock_device_server.py --self-test
git diff --check
git status --short
```

Expected: all Python tests and self-test pass, no whitespace errors are reported, and only intentional uncommitted files remain.
