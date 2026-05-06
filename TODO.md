# Origami Fold — TODO

Goal: turn the current "fold a single sheet" prototype into a real
origami workbench: per-page folding, measurement aids, exportable
output, an auto-generated fold guide, a 3D preview, and basic paper
physics (pull-out / detach).

Phases are ordered so each one stands on its own — you can stop after
any phase and still have a usable build.

---

## Phase 1 — Per-page (per-layer) folding
Currently `origami_paper_fold` walks every layer in the stack. We need
to fold only the selected page instead of always the whole stack.

- [x] Give every layer a stable id (so undo/history can refer to it).
- [x] Add a "fold target" mode on the canvas: `ALL_LAYERS` or
      `SELECTED_LAYER`.
- [x] Hover-highlight the topmost layer under the cursor.
- [x] Click-to-select a single layer in selected-layer mode.
- [x] Update `origami_paper_fold` to take a target-layer id (NULL = all).
- [x] Header-bar toggle button: "Fold all" vs "Fold page".

## Phase 2 — Rulers and measurement aids
- [x] Draw horizontal and vertical rulers around the canvas, in mm
      (paper coordinates).
- [x] Live readout while placing a fold line: length (mm) and angle
      (deg) from horizontal.
- [x] Snap helpers: hold Shift to snap angle to multiples of 15°, hold
      Ctrl to snap endpoint to nearest vertex / edge midpoint of any
      visible layer.
- [x] Display the fold line's distance to the nearest paper edge.

## Phase 3 — Export
- [x] Export current view to PNG (via GtkFileDialog).
- [x] Export current view to SVG (Cairo SVG surface).
- [x] Export fold-by-fold instructions to a Markdown file.

## Phase 4 — Fold history → instruction panel
- [x] Record each fold operation: target layer, line endpoints, fold
      side, plus a short auto-generated description ("Step 3: fold the
      top-right page diagonally at 45°").
- [x] Side panel listing the steps in order; click a step to restore
      the paper to that state.
- [x] Step description uses the rulers so it's reproducible by hand
      ("from x=12mm,y=0mm to x=120mm,y=80mm").

## Phase 5 — 3D preview
- [x] Add a `Adw.ViewStack` so the window has a "Design" view (current
      2D editor) and a "3D Preview" view.
- [x] In the 3D view render each layer as a polygon at a small z-offset
      proportional to its stack position, so the layered structure is
      visible. Cairo-based fake-isometric is fine; OpenGL is a stretch.
- [x] Mouse drag to orbit; scroll to zoom.

## Phase 6 — Paper physics / pull-out
Stretch goal — keep the surface area small.

- [x] "Pull" tool: pick a layer, translate/rotate it freely without
      reflecting. Models lifting a flap or pulling a tab.
      (Drag = translate, Shift+drag = rotate.)
- [ ] Detach: cut a layer free of the rest. Skipped — per-page folding
      already lets each layer be folded independently, and a true
      "make this a separate sheet" operation would need a second
      paper instance which this UI doesn't accommodate.
- [x] In the 3D view show the pulled layer offset out of the stack
      (any layer with a non-zero free transform gets an extra lift in
      the projection).

## Phase 7 — Polish
- [x] Keyboard shortcut cheatsheet — included in the About dialog.
- [ ] Persist last-used fold mode and ruler visibility across runs.
      Skipped — would need a GSettings schema or an XDG keyfile, both
      out of scope for this pass.
- [ ] Palette picker for paper front/back colors. Skipped.

---

## Notes for future work

- The fold operation only tracks 2D state. A genuine 3D unfold/refold
  preview would need a crease graph (which polygon shares which edge
  with which). The current layer model can't reconstruct that.
- The Pull tool baks free transforms into geometry on the next fold of
  the same layer, so transforms don't compound infinitely with folds.
