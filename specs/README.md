## 1. Controls

### 1.1 Tool keys (ctrl + letter)

`ctrl+c` is the program's quit key and is **never** used by a tool.

| Keys   | Tool            | Kind            | Drawing action                              |
|--------|-----------------|-----------------|---------------------------------------------|
| ctrl+x | Eraser          | hold            | erase (paint spaces) while dragging          |
| ctrl+l | Line            | shape (toggle)  | anchor point to end point                    |
| ctrl+r | Rectangle       | shape (toggle)  | corner to opposite corner, outline           |
| ctrl+f | Filled rect     | shape (toggle)  | corner to opposite corner, filled            |
| ctrl+e | Ellipse         | shape (toggle)  | fitted in the dragged bounding box, outline  |
| ctrl+o | Filled ellipse  | shape (toggle)  | fitted in the dragged bounding box, filled   |

The pencil needs no key: it is the tool at startup, it returns after every
shape commit and after `ctrl+n`, and pressing the key of an active tool
again disarms back to it.

The terminal window title always shows the active tool
(`Paint - tool: Line`, ...), and after a save / new-canvas / undo / redo
action it also reports that status until the next tool change (see 1.4
and 1.5).

### 1.2 Keyboard

| Input           | Action                                                        |
|-----------------|---------------------------------------------------------------|
| any printable   | pick it as the brush character, paint it, advance like a typewriter |
| arrow keys      | move the text cursor one cell                                 |
| backspace       | clear the cell under the cursor                               |
| ctrl+p          | export the canvas to TypeInk.txt (see 1.4)                    |
| ctrl+n          | wipe the canvas and start a new drawing (see 1.5)             |
| ctrl+z          | undo the last action (see 1.5)                                |
| ctrl+y          | redo the last undone action (see 1.5)                         |
| Esc             | quit                                                          |
| ctrl+c          | quit (reserved by the input layer)                            |

Notes:

- Typing while the **eraser** is active switches back to the pencil.
- The brush character applies to pencil strokes *and* to shapes (a `#`
  brush draws `#`-shaped lines and ellipses). A space brush makes shapes
  invisible — preview and commit are both blank, which is consistent but
  pointless; pick a visible character.
- `ctrl+letter` chords never reach the typing path: the input layer sets
  `keys.ctrl` and the letter flag, leaving `keys.lastChar` untouched, so
  tool keys can never paint characters.

### 1.3 Mouse

Every mouse move is tracked (any-motion mode), and the text cursor follows
the pointer, clamped to the canvas. The arrow pointer is requested via
OSC 22 and restored on exit.

**Hold tools (pencil, eraser):** press and hold the left button, drag to
paint, release. The stroke is committed to the canvas live, in white.

**Shape tools (line, rectangle, filled rect, ellipse, filled ellipse):**
toggle-based, no holding required:

1. `ctrl+<letter>` **arms** the tool — the title updates.
2. **First left click** anchors the shape at that cell.
3. **Moving the mouse** traces the shape **live in grey** (`\033[90m`).
   This is only a preview; nothing is committed yet, and the rest of the
   canvas stays white and untouched.
4. **Second left click** commits the shape into the canvas in the default
   white, at the exact release point; the grey preview disappears; the tool
   falls back to the pencil (one shape per arming).
5. Pressing the same key again *before* anchoring disarms back to pencil.
6. Pressing any tool key **while a trace is live** cancels the preview
   without committing; the toggle rule then applies (same key → pencil,
   other key → that tool).

A short "latch" after the commit click swallows the tail of that same
button press, so committing can never smear stray pencil pixels.

### 1.4 Export (ctrl+p)

`ctrl+p` types the whole canvas buffer into **`TypeInk.txt`** in the
current working directory (the directory `./paint` was started from), so
the exact drawing from the terminal exists as a plain text file.

- Built on the engine's `getBuffer()`: one line per terminal row,
  character for character what the canvas holds. Interior spacing is
  preserved exactly; trailing spaces at the end of a line are dropped
  because they are invisible on screen.
- The file is **overwritten** on every export — nothing is appended.
- Export works at any time, including mid-trace: the grey shape preview is
  not committed, so it is not part of the canvas and not exported.
- The result is confirmed in the window title: `Paint - tool: <tool> -
  saved TypeInk.txt`, or `... - save failed` if the file could not be
  opened (read-only directory, bad path, ...). The note is retired by the
  next tool change.

### 1.5 New canvas (ctrl+n) and history (ctrl+z / ctrl+y)

**`ctrl+n`** wipes the whole display and starts from a blank canvas: the
tool falls back to the pencil and the title confirms `... - new canvas`.
The wiped picture is recorded first, so `ctrl+z` brings it straight back.

**`ctrl+z` / `ctrl+y`** are whole-canvas undo / redo with the standard
editor rules:

- Every canvas-mutating action records the buffer (`engine.getBuffer()`)
  **before** it changes anything. One action = one undo step:
  - a pencil / eraser drag records **once at its first dab** — a whole
    stroke is one step, not one step per pixel,
  - a shape records at its commit click (the grey preview is not canvas
    state and is never recorded),
  - each typed character, each backspace and each `ctrl+n` records per
    press.
- Undo steps back one record; redo steps forward again. Alternating
  undo/redo walks back and forth through the whole history, capped at
  100 steps (the oldest is dropped beyond that).
- Any new action discards the redo branch, exactly like a text editor.
- A record identical to the newest one is skipped, so holding backspace
  over empty cells or repeating `ctrl+n` on an unchanged canvas does not
  flood the history with no-op steps.
- Undo / redo cancel a grey shape trace in progress, so no stale preview
  survives a restore. The title reports `... - undo`, `... - redo`,
  `... - nothing to undo` / `... - nothing to redo` until the next tool
  change.

Restoration rewrites every cell of the canvas through `setChar`. Colors
are per-render (one default color per frame), so characters alone are
enough to rebuild the frame exactly.

---

## 2. Behavior contract of the trace layer

This is the part that was reworked after the "everything turns grey" bug,
and it is worth understanding before changing anything:

- `engine.render()` repaints the **whole screen in one color** — the
  engine's `defaultColor`. Two full renders can never be layered; the
  second render's spaces erase the first. That is why the old approach
  (copying the canvas into a grey-rendered trace engine) greyed out the
  entire display.
- The layering is therefore explicit:
  1. `e.render()` runs **every frame** — the committed canvas, in the
     default white (`e.setDefaultColor("#FFFFFF")`).
  2. The `top` engine holds **only the cells of the shape being traced** —
     never a copy of the canvas.
  3. After the canvas render, `paintTrace()` writes **just those cells**,
     in grey, at their own cursor positions. Every other cell of the
     screen is untouched, so the canvas below stays white.
  4. On commit, `commitTrace()` burns the trace cells into `e` via
     `setChar` (they then render white), the layer is cleared, and the
     next full repaint erases the grey preview. Cancel takes the same path
     (just clear + repaint).
- Preview and commit are **pixel-identical by construction**: both go
  through the same rasterizers, and the committed pixels are literally the
  previewed cells copied from `top` into `e`.

### Engine API used (the full contract)

| API                              | Used for                                              |
|----------------------------------|-------------------------------------------------------|
| `rows`, `cols`                   | bounds, loops, cursor clamping                        |
| `setChar(row, col, c)`           | pencil/eraser/typing, staging the trace, committing   |
| `getChar(row, col)`              | reading trace cells for commit and grey overlay       |
| `getBuffer()`                    | whole canvas as one string: ctrl+p export, ctrl+z/ctrl+y snapshots |
| `setDefaultColor("#RRGGBB")`     | explicit white canvas color                           |
| `render()`                       | the one full-screen white frame per loop pass         |

The trace grey is emitted by the application (`\033[90m`) because the
engine exposes no color getter; the engine API itself is used exactly as
listed above.

### Input-layer facts the code relies on

- `ctrl+letter` sets `keys.ctrl` **and the letter flag** (`keys.l`,
  `keys.e`, ...), and does **not** set `keys.lastChar`.
- `ctrl+h` / `ctrl+i` / `ctrl+j` / `ctrl+m` are consumed by the terminal as
  backspace / tab / linefeed / enter and must not be used as tool keys.
- `mouse.pressed` is a single-frame edge — it drives both the anchor and
  the commit click.
- `mouse.motion` is true while the pointer moves (any-motion tracking).
- `ctrl+n` / `ctrl+p` / `ctrl+z` / `ctrl+y` set their letter flags like any
  other chord; the application consumes them in the main loop (canvas,
  export, history), not in the tool selector.

---

## 3. Code map of `main.cpp`

| Section | Contents                                                             |
|---------|----------------------------------------------------------------------|
| 1       | `Tool` enum, `toolName`, `isShapeTool`, `showTool` (OSC 2 title)      |
| 2       | `clearGrid`                                                          |
| 3       | Rasterizers: `drawLineShape` (Bresenham), `drawRectShape`, `drawEllipseShape`, `drawShape` dispatcher |
| 4       | Trace layer: `commitTrace`, `paintTrace`                              |
| 5       | `PaintState` + one handler per input source: `selectTool`, `exportCanvas` (+ `exportCanvasFile`), `newCanvas`, `undoStep` / `redoStep` (+ history helpers `pushUndo`, `restoreBuffer`, `cancelTrace`), `updateCursor`, `handleShapeClicks`, `paintFreehand`, `moveCursorKeys`, `typeCharacters`, `stageTrace` |
| 6       | `main()` — the frame pipeline                                         |

The frame pipeline, once per loop pass:

```
t.update()
  │
  ├─ selectTool        ctrl+letter chords: toggle / switch / cancel trace
  ├─ exportCanvas      ctrl+p: canvas buffer -> TypeInk.txt
  ├─ newCanvas         ctrl+n: record + wipe the canvas, back to pencil
  ├─ undoStep          ctrl+z: restore the previous recorded state
  ├─ redoStep          ctrl+y: restore the last undone state
  ├─ updateCursor      mouse -> text cursor (clamped)
  ├─ handleShapeClicks anchor click / commit click (records before commit)
  ├─ latch release     commit click tail is swallowed; drag = one undo step
  ├─ paintFreehand     pencil / eraser drag painting (records at first dab)
  ├─ moveCursorKeys    arrow keys
  ├─ typeCharacters    brush character + paint + advance (records per char)
  ├─ backspace         record + clear cell
  │
  ├─ stageTrace        pending shape -> trace engine (only if tracing)
  ├─ e.render()        full screen, white canvas
  └─ paintTrace        grey sparse overlay (only if tracing)
```

---

## 4. Extending

### 4.1 Add a new shape tool (example: triangle)

1. Add `triangle` to the `Tool` enum.
2. Add a `"Triangle"` case to `toolName()`.
3. Add `Tool::triangle` to `isShapeTool()` — this gives it the
   arm → anchor → grey trace → click-commit flow for free.
4. Add a key branch in `selectTool()`, e.g. `t.keys.g` (use a letter that
   is not consumed by the terminal: avoid `h`, `i`, `j`, `m`; never use `c`
   (quit) or `n` / `p` / `z` / `y` (new canvas / save / undo / redo)).
5. Add a `drawTriangleShape()` rasterizer plus a case in `drawShape()`.
6. Add a row to the controls table in this README.

Nothing else changes — arming, title, grey preview, commit and cancel all
come from the shared pipeline.

### 4.2 Add a hold-based tool (like the eraser)

Same as above, but do **not** add it to `isShapeTool()`; instead paint in
`paintFreehand()` under a new `else if`. Typing-to-exit behavior can be
mirrored from the eraser in `typeCharacters()`.

### 4.3 Extension ideas

- **Span-level undo** — history is whole-canvas snapshots (`getBuffer()`);
  `{row, col, before, after}` span records would shrink the memory per step
  and open the door to per-cell undo in huge terminals.
- **Save / load** — saving exists (`ctrl+p` exports `TypeInk.txt` via
  `getBuffer()`); a loader can read that file back line by line and paint
  it with `setChar`. `exportCanvasFile()` is also the natural hook for
  other formats (JSON, ANSI with color, ...).
- **Canvas colors** — `setDefaultColor()` is already wired up; map extra
  keys to palettes. Remember the color is per-render, so two colors in one
  frame would reuse the `paintTrace` compositing pattern.
- **Status bar** — render hints (active tool, brush char, cursor position)
  into a reserved screen row using the same sparse-overlay technique.
- **Resize** — the engine reads the terminal size once at startup; if it
  ever exposes a resize/re-init, rebuild `e` and `top` on `SIGWINCH`.

### 4.4 Known limitations

- Terminal resize is not handled (the engine sizes once at construction).
- A space brush character makes shapes invisible (by design, both preview
  and commit use the same character).
- One color per `render()` — the grey preview exists only through the
  sparse overlay; anything richer needs the same compositing pattern.
- Requires a terminal with mouse reporting enabled (`?1003` any-motion)
  and OSC 2 / OSC 22 support.
- The export target is fixed: `TypeInk.txt` in the current working
  directory, overwritten on each export.
- Undo history is memory-only (it dies with the process) and capped at
  100 whole-canvas snapshots.
