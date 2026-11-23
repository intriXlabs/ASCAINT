// ============================================================================
// Terminal Paint - drawing tools on top of the control + engine modules
// ============================================================================
//
// This file is the only part of the project meant to be edited. It consumes
// two provided modules strictly through their exposed APIs:
//
//   control/main.cpp  - raw terminal input: keyboard chords, mouse state,
//                       write() for raw escape-sequence output
//   engine/main.cpp   - character framebuffer: rows, cols, setChar, getChar,
//                       getBuffer, setDefaultColor, render()
//
// Full documentation of every control and behaviour lives in README.md next
// to this file. Map of this file:
//
//   1. Tools             - enum, names, arming rules, window title display
//   2. Grid helpers      - clearGrid
//   3. Shape rasterizers - line / rectangle / ellipse (+ filled), drawShape
//   4. Trace layer       - commitTrace, paintTrace (the grey live preview)
//   5. App state         - PaintState + one handler per input source,
//                          incl. ctrl+p export, ctrl+n new canvas and
//                          ctrl+z / ctrl+y undo / redo
//   6. Main loop         - the frame pipeline
//
// ---------------------------------------------------------------------------
// Rendering model (why the grey preview can never grey out the canvas)
// ---------------------------------------------------------------------------
// engine.render() repaints the WHOLE screen in ONE color, so two full
// renders can never be layered - the second render's spaces erase the
// first. The layering is therefore done explicitly:
//
//   1. e.render() runs EVERY frame - the committed canvas, default white
//   2. the "top" engine holds ONLY the cells of the shape being traced,
//      never a copy of the canvas
//   3. paintTrace() writes just those cells, in grey, at their own cursor
//      positions - every other cell of the screen stays untouched
//   4. commitTrace() burns the cells into "e" (they then render white) and
//      the next full repaint erases the grey preview
//
// ---------------------------------------------------------------------------
// Input-layer contract (why the code checks what it checks)
// ---------------------------------------------------------------------------
// - ctrl+letter chords set keys.ctrl and the letter flag (keys.l, keys.e,
//   ...) and leave keys.lastChar untouched: tool selection checks the
//   letter flags, never lastChar.
// - ctrl+c is the program's quit key - it is NEVER used by a tool here.
// - ctrl+n / p / z / y are app-level chords (new canvas, save, undo, redo)
//   handled in the main loop - they are not tool selections.
// - ctrl+h / i / j / m are consumed by the terminal (backspace/tab/LF/enter).
// - mouse.pressed is a single-frame edge: it drives anchor and commit clicks.
// - "latch" suppresses pencil paint until the commit click is released.
//
// ---------------------------------------------------------------------------
// Extending (short version - README.md has the detailed recipe)
// ---------------------------------------------------------------------------
// A new shape tool = 1 enum value + 1 name + 1 isShapeTool entry + 1 key
// branch in selectTool() + 1 case in drawShape(). Arming, anchoring, the
// grey trace, commit and title display all come free from the pipeline.
// Any code path that mutates the canvas calls pushUndo() first, so the
// action is one ctrl+z step.
// ============================================================================

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "control/main.cpp"
#include "engine/main.cpp"

// ---------------------------------------------------------------------------
// 1. Tools
//
// Shape tools are TOGGLE based: ctrl+letter arms the tool for one shape
// (pressing the same key again disarms back to pencil), the first left
// click anchors, the mouse traces the shape live in grey, the second click
// commits. Pencil and eraser are HOLD based: they paint while the button
// is down. The active tool is shown in the terminal title (OSC 2).
// ---------------------------------------------------------------------------

enum class Tool {
    pencil,            // paint the current brush character while dragging
    eraser,            // paint spaces while dragging
    line,              // anchor -> opposite end
    rectangle,         // corner -> opposite corner
    filledRectangle,
    ellipse,           // bounding-box corner -> opposite corner
    filledEllipse
};

static const char* toolName(Tool tool) {
    switch (tool) {
        case Tool::pencil:          return "Pencil";
        case Tool::eraser:          return "Eraser";
        case Tool::line:            return "Line";
        case Tool::rectangle:       return "Rectangle";
        case Tool::filledRectangle: return "Filled rectangle";
        case Tool::ellipse:         return "Ellipse";
        case Tool::filledEllipse:   return "Filled ellipse";
    }

    return "Pencil";
}

// Shape tools use the click -> trace -> click flow; freehand tools do not
static bool isShapeTool(Tool tool) {
    return tool == Tool::line || tool == Tool::rectangle ||
           tool == Tool::filledRectangle || tool == Tool::ellipse ||
           tool == Tool::filledEllipse;
}

// Show the active tool in the terminal window title (OSC 2). "note" carries
// a transient status behind the tool name (save / new canvas / undo / redo
// results); it is cleared by the next tool change.
static void showTool(terminalControl& t, Tool tool, const std::string& note = "") {
    t.write("\033]2;Paint - tool: ");
    t.write(toolName(tool));
    t.write(note.c_str());
    t.write("\033\\");
}

// ---------------------------------------------------------------------------
// 2. Grid helpers (exposed engine API only)
// ---------------------------------------------------------------------------

// Reset a grid to all spaces (the engine has no clear method of its own)
static void clearGrid(engine& g) {
    for (int r = 0; r < g.rows; ++r)
        for (int c = 0; c < g.cols; ++c)
            g.setChar(r, c, ' ');
}

// ---------------------------------------------------------------------------
// 3. Shape rasterizers
//
// Every rasterizer takes a target engine, so the same code both previews a
// shape into the trace layer and (through the trace layer) commits it into
// the canvas - preview and commit are pixel-identical by construction.
// ---------------------------------------------------------------------------

static int iabs(int v) {
    return v < 0 ? -v : v;
}

// setChar with clipping to the grid
static void putChar(engine& e, int row, int col, char ch) {
    if (row < 0 || row >= e.rows || col < 0 || col >= e.cols)
        return;

    e.setChar(row, col, ch);
}

// Bresenham line
static void drawLineShape(engine& e, int r0, int c0, int r1, int c1, char ch) {
    int dr = iabs(r1 - r0);
    int dc = iabs(c1 - c0);
    int sr = r0 < r1 ? 1 : -1;
    int sc = c0 < c1 ? 1 : -1;
    int err = dc - dr;

    while (true) {
        putChar(e, r0, c0, ch);

        if (r0 == r1 && c0 == c1)
            break;

        int err2 = 2 * err;

        if (err2 > -dr) {
            err -= dr;
            c0 += sc;
        }

        if (err2 < dc) {
            err += dc;
            r0 += sr;
        }
    }
}

// Rectangle, outline or filled
static void drawRectShape(engine& e, int r0, int c0, int r1, int c1, char ch, bool filled) {
    if (r0 > r1) { int tmp = r0; r0 = r1; r1 = tmp; }
    if (c0 > c1) { int tmp = c0; c0 = c1; c1 = tmp; }

    if (filled) {
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
                putChar(e, r, c, ch);

        return;
    }

    for (int c = c0; c <= c1; ++c) {
        putChar(e, r0, c, ch);
        putChar(e, r1, c, ch);
    }

    for (int r = r0; r <= r1; ++r) {
        putChar(e, r, c0, ch);
        putChar(e, r, c1, ch);
    }
}

// Ellipse fitted in the bounding box r0,c0 - r1,c1, outline or filled
static void drawEllipseShape(engine& e, int r0, int c0, int r1, int c1, char ch, bool filled) {
    if (r0 > r1) { int tmp = r0; r0 = r1; r1 = tmp; }
    if (c0 > c1) { int tmp = c0; c0 = c1; c1 = tmp; }

    int height = r1 - r0;
    int width  = c1 - c0;

    // Degenerate boxes fall back to a point or a straight line
    if (height == 0 && width == 0) {
        putChar(e, r0, c0, ch);
        return;
    }

    if (height == 0) {
        drawLineShape(e, r0, c0, r0, c1, ch);
        return;
    }

    if (width == 0) {
        drawLineShape(e, r0, c0, r1, c0, ch);
        return;
    }

    double rx = width  / 2.0;          // horizontal radius
    double ry = height / 2.0;          // vertical radius
    double cr = (r0 + r1) / 2.0;       // center row
    double cc = (c0 + c1) / 2.0;       // center column

    if (filled) {
        for (int r = r0; r <= r1; ++r) {
            double dy = (r - cr) / ry;
            double t = 1.0 - dy * dy;

            if (t < 0.0)
                t = 0.0;

            int half = (int)std::lround(rx * std::sqrt(t));
            int left = (int)std::lround(cc - half);
            int right = (int)std::lround(cc + half);

            for (int c = left; c <= right; ++c)
                putChar(e, r, c, ch);
        }

        return;
    }

    // Outline: the row extremes give the left/right edges and the column
    // extremes give the top/bottom edges; together they leave no gaps.
    for (int r = r0; r <= r1; ++r) {
        double dy = (r - cr) / ry;
        double t = 1.0 - dy * dy;

        if (t < 0.0)
            continue;

        int half = (int)std::lround(rx * std::sqrt(t));
        putChar(e, r, (int)std::lround(cc - half), ch);
        putChar(e, r, (int)std::lround(cc + half), ch);
    }

    for (int c = c0; c <= c1; ++c) {
        double dx = (c - cc) / rx;
        double t = 1.0 - dx * dx;

        if (t < 0.0)
            continue;

        int half = (int)std::lround(ry * std::sqrt(t));
        putChar(e, (int)std::lround(cr - half), c, ch);
        putChar(e, (int)std::lround(cr + half), c, ch);
    }
}

// Draw the shape of the given tool into an engine - the trace layer while
// the shape is being made, never the canvas directly
static void drawShape(engine& target, Tool tool, int r0, int c0, int r1, int c1, char ch) {
    switch (tool) {
        case Tool::line:
            drawLineShape(target, r0, c0, r1, c1, ch);
            break;

        case Tool::rectangle:
            drawRectShape(target, r0, c0, r1, c1, ch, false);
            break;

        case Tool::filledRectangle:
            drawRectShape(target, r0, c0, r1, c1, ch, true);
            break;

        case Tool::ellipse:
            drawEllipseShape(target, r0, c0, r1, c1, ch, false);
            break;

        case Tool::filledEllipse:
            drawEllipseShape(target, r0, c0, r1, c1, ch, true);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 4. Trace layer (exposed API only: engine.setChar/getChar + t.write)
// ---------------------------------------------------------------------------

// Burn the trace layer into the canvas: every cell that holds a shape
// character is committed into the normal engine, so it renders in the
// default color. This is the ONLY path a shape takes onto the canvas.
static void commitTrace(engine& e, const engine& top) {
    for (int r = 0; r < top.rows; ++r)
        for (int c = 0; c < top.cols; ++c) {
            char ch = top.getChar(r, c);

            if (ch != ' ')
                e.setChar(r, c, ch);
        }
}

// Composite the trace layer over the canvas frame: paint ONLY the cells
// that hold a trace character, in grey (\033[90m), at their own cursor
// positions. The rest of the screen - the white canvas below - is not
// touched, so the canvas never turns grey while a shape is being made.
static void paintTrace(terminalControl& t, const engine& top) {
    std::string out;
    out.reserve(8192);

    for (int r = 0; r < top.rows; ++r) {
        int c = 0;

        while (c < top.cols) {
            // skip cells the trace does not occupy
            while (c < top.cols && top.getChar(r, c) == ' ')
                ++c;

            if (c >= top.cols)
                break;

            // one run of consecutive trace cells shares one cursor position
            int run = c;
            while (c < top.cols && top.getChar(r, c) != ' ')
                ++c;

            out += "\033[";
            out += std::to_string(r + 1);
            out += ';';
            out += std::to_string(run + 1);
            out += "H\033[90m";

            for (int i = run; i < c; ++i)
                out += top.getChar(r, i);
        }
    }

    out += "\033[0m";
    t.write(out.c_str());
}

// ---------------------------------------------------------------------------
// 5. Application state and input handlers
//
// PaintState owns everything the loop mutates. Every input source gets one
// handler, so new inputs slot in without touching the other handlers -
// including the ctrl+p export and the ctrl+n / ctrl+z / ctrl+y commands.
// ---------------------------------------------------------------------------

struct PaintState {
    Tool tool = Tool::pencil;
    bool shaping = false;     // anchor registered, grey trace is live
    bool latch = false;       // ignore pencil paint until commit click release
    bool strokeOpen = false;  // undo snapshot taken for the current drag
    int startRow = 0;         // anchor of the shape currently being traced
    int startCol = 0;
    int row = 0;              // text cursor (follows mouse + arrow keys)
    int col = 0;
    char lastChar = '#';      // brush character, set by typing any printable
    std::string note;         // transient title status (save / new / undo)
    std::vector<std::string> undoStack;   // canvas state before each action
    std::vector<std::string> redoStack;   // states stepped back out by undo
};

// Type the whole canvas buffer into a file: one line per terminal row,
// character for character what the canvas holds (engine.getBuffer()).
// Interior spacing is preserved exactly; trailing spaces at the end of a
// line are dropped because they are invisible on screen. The file is
// overwritten on every export.
static bool exportCanvasFile(const engine& e, const std::string& path) {
    std::string buf = e.getBuffer();   // rows*cols chars, row-major, no newlines

    std::string out;
    out.reserve(buf.size() + (size_t)e.rows + 16);

    for (int r = 0; r < e.rows; ++r) {
        size_t rowStart = (size_t)r * (size_t)e.cols;
        size_t len = (size_t)e.cols;

        // defensive clamp in case the buffer is shorter than rows*cols
        if (rowStart + len > buf.size())
            len = buf.size() > rowStart ? buf.size() - rowStart : 0;

        // drop this line's trailing spaces - invisible in the drawing
        while (len > 0 && buf[rowStart + len - 1] == ' ')
            --len;

        // len == 0 (blank line) or the defensive clamp left nothing: write
        // just the newline - append() would throw on pos > size()
        if (len > 0)
            out.append(buf, rowStart, len);

        out += '\n';
    }

    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;

    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

// ctrl+p: type the whole committed canvas into TypeInk.txt in the current
// working directory, so the exact drawing from the terminal lands in a
// plain text file. Works at any time; a grey trace in progress is only a
// preview and is not committed, so it is not exported. The result is shown
// in the window title until the next tool change.
static void exportCanvas(terminalControl& t, PaintState& s, const engine& e) {
    if (!t.keys.ctrl || !t.keys.p)
        return;

    s.note = exportCanvasFile(e, "TypeInk.txt") ? " - saved TypeInk.txt"
                                                : " - save failed";
    showTool(t, s.tool, s.note);
}

// ---------------------------------------------------------------------------
// History: ctrl+z undo / ctrl+y redo / ctrl+n new canvas
//
// Every canvas-mutating action records the whole buffer (getBuffer()) BEFORE
// it changes anything: a freehand stroke records once at its first dab, a
// shape at its commit, a typed character / backspace / ctrl+n per press.
// Undo pops the newest record and writes it back cell by cell; redo keeps
// what undo steps back out of. Any new action discards the redo branch,
// exactly like a text editor.
// ---------------------------------------------------------------------------

static const size_t kMaxHistorySteps = 100;   // memory cap for both stacks

// Record the pre-action canvas. A record identical to the newest one is
// skipped, so repeated identical states (spamming ctrl+n or backspace over
// cells that are already empty) never consume undo steps.
static void pushUndo(PaintState& s, const engine& e) {
    std::string snap = e.getBuffer();

    if (!s.undoStack.empty() && s.undoStack.back() == snap)
        return;

    s.undoStack.push_back(snap);

    if (s.undoStack.size() > kMaxHistorySteps)
        s.undoStack.erase(s.undoStack.begin());   // drop the oldest step

    s.redoStack.clear();     // a fresh action invalidates the redo branch
}

// Write a recorded buffer back into the engine, one cell at a time
static void restoreBuffer(engine& e, const std::string& buf) {
    size_t need = (size_t)e.rows * (size_t)e.cols;

    if (buf.size() < need)   // defensive: never index past the snapshot
        return;

    for (int r = 0; r < e.rows; ++r)
        for (int c = 0; c < e.cols; ++c)
            e.setChar(r, c, buf[(size_t)r * (size_t)e.cols + (size_t)c]);
}

// Abandon a half-made shape so no stale grey preview survives a restore
static void cancelTrace(PaintState& s, engine& top) {
    if (s.shaping) {
        s.shaping = false;
        clearGrid(top);
    }
}

// ctrl+z: revert the last action
static void undoStep(terminalControl& t, PaintState& s, engine& e, engine& top) {
    if (!t.keys.ctrl || !t.keys.z)
        return;

    if (s.undoStack.empty()) {
        s.note = " - nothing to undo";
        showTool(t, s.tool, s.note);
        return;
    }

    s.redoStack.push_back(e.getBuffer());   // current state becomes redoable
    restoreBuffer(e, s.undoStack.back());
    s.undoStack.pop_back();

    cancelTrace(s, top);
    s.note = " - undo";
    showTool(t, s.tool, s.note);
}

// ctrl+y: re-apply the last undone action
static void redoStep(terminalControl& t, PaintState& s, engine& e, engine& top) {
    if (!t.keys.ctrl || !t.keys.y)
        return;

    if (s.redoStack.empty()) {
        s.note = " - nothing to redo";
        showTool(t, s.tool, s.note);
        return;
    }

    s.undoStack.push_back(e.getBuffer());   // the undo itself becomes undoable
    restoreBuffer(e, s.redoStack.back());
    s.redoStack.pop_back();

    cancelTrace(s, top);
    s.note = " - redo";
    showTool(t, s.tool, s.note);
}

// ctrl+n: wipe the whole display and start from a new, blank canvas. The
// wiped picture is recorded first, so ctrl+z brings it straight back.
static void newCanvas(terminalControl& t, PaintState& s, engine& e, engine& top) {
    if (!t.keys.ctrl || !t.keys.n)
        return;

    pushUndo(s, e);
    clearGrid(e);
    cancelTrace(s, top);

    s.tool = Tool::pencil;   // a new canvas starts with the basic tool
    s.note = " - new canvas";
    showTool(t, s.tool, s.note);
}

// ctrl+letter tool selection, toggle based (never ctrl+c - that quits).
// Any requested chord cancels a trace in progress; the key of the active
// tool disarms back to pencil, any other key switches tools. ctrl+n is
// NOT a tool key - it wipes the canvas (newCanvas above); the pencil needs
// no key, it is the default and always returns on its own.
static void selectTool(terminalControl& t, PaintState& s, engine& top) {
    if (!t.keys.ctrl)
        return;

    Tool selected = s.tool;
    bool requested = false;

    if (t.keys.l)      { selected = Tool::line;            requested = true; }
    else if (t.keys.e) { selected = Tool::ellipse;         requested = true; }
    else if (t.keys.r) { selected = Tool::rectangle;       requested = true; }
    else if (t.keys.f) { selected = Tool::filledRectangle; requested = true; }
    else if (t.keys.o) { selected = Tool::filledEllipse;   requested = true; }
    else if (t.keys.x) { selected = Tool::eraser;          requested = true; }

    if (!requested)
        return;

    // any tool change cancels a shape trace in progress
    if (s.shaping) {
        s.shaping = false;
        clearGrid(top);
    }

    Tool next = (s.tool == selected) ? Tool::pencil : selected;

    if (next != s.tool) {
        s.tool = next;
        s.note.clear();          // a tool change retires the save status
        showTool(t, s.tool, s.note);
    }
}

// Follow the mouse with the text cursor, clamped to the canvas
static void updateCursor(const terminalControl& t, PaintState& s, const engine& e) {
    if (!t.mouse.motion && !t.mouse.pressed)
        return;

    s.row = t.mouse.y;
    s.col = t.mouse.x;

    if (s.row < 0)      s.row = 0;
    if (s.col < 0)      s.col = 0;
    if (s.row >= e.rows) s.row = e.rows - 1;
    if (s.col >= e.cols) s.col = e.cols - 1;
}

// Stage the pending shape into the trace layer - only the shape's own
// cells go in here, never a copy of the canvas
static void stageTrace(const PaintState& s, engine& top) {
    clearGrid(top);
    drawShape(top, s.tool, s.startRow, s.startCol, s.row, s.col, s.lastChar);
}

// Shape tools: the first left click anchors the shape, the second click
// commits it. Pencil/eraser clicks are ignored here.
static void handleShapeClicks(terminalControl& t, PaintState& s, engine& e, engine& top) {
    if (!t.mouse.pressed || !t.mouse.left)
        return;

    if (!s.shaping && isShapeTool(s.tool)) {
        s.startRow = s.row;
        s.startCol = s.col;
        s.shaping = true;
        return;
    }

    if (s.shaping) {
        // final trace at the exact release point, then burn the cells into
        // the canvas in the default color and reset the grey layer
        stageTrace(s, top);
        pushUndo(s, e);     // record the canvas WITHOUT the shape first
        commitTrace(e, top);
        clearGrid(top);

        s.shaping = false;
        s.tool = Tool::pencil;  // one shape per arming
        s.note.clear();         // a tool change retires the save status
        showTool(t, s.tool, s.note);
        s.latch = true;
    }
}

// Pencil and eraser paint while the left button is held; "latch" blocks
// the tail of a commit click from smearing pixels. The first dab of a drag
// records the pre-stroke canvas, so one whole stroke is one undo step.
static void paintFreehand(PaintState& s, engine& e, const terminalControl& t) {
    if (s.latch || !t.mouse.left || !t.mouse.motion)
        return;

    if (s.tool != Tool::pencil && s.tool != Tool::eraser)
        return;              // shape tools only trace grey here, never paint

    if (!s.strokeOpen) {
        pushUndo(s, e);      // exactly once per drag, BEFORE the first dab
        s.strokeOpen = true;
    }

    if (s.tool == Tool::pencil)
        e.setChar(s.row, s.col, s.lastChar);
    else
        e.setChar(s.row, s.col, ' ');   // eraser
}

// Arrow-key navigation of the text cursor
static void moveCursorKeys(const terminalControl& t, PaintState& s, const engine& e) {
    if (t.keys.up && s.row > 0)
        --s.row;

    if (t.keys.down && s.row < e.rows - 1)
        ++s.row;

    if (t.keys.left && s.col > 0)
        --s.col;

    if (t.keys.right && s.col < e.cols - 1)
        ++s.col;
}

// Printable characters pick the brush character and paint it at the
// cursor, then advance like a typewriter. ctrl chords never set lastChar,
// so this cannot fire while a tool key is held. Typing leaves eraser mode.
static void typeCharacters(terminalControl& t, PaintState& s, engine& e) {
    if (!t.keys.lastChar || t.keys.ctrl || t.keys.alt)
        return;

    s.lastChar = t.keys.lastChar;

    if (s.tool == Tool::eraser) {
        s.tool = Tool::pencil;   // typing a character leaves eraser mode
        s.note.clear();          // a tool change retires the save status
        showTool(t, s.tool, s.note);
    }

    pushUndo(s, e);          // one undo step per typed character
    e.setChar(s.row, s.col, s.lastChar);

    if (s.col < e.cols - 1)
        ++s.col;
    else if (s.row < e.rows - 1) {
        s.col = 0;
        ++s.row;
    }
}

// ---------------------------------------------------------------------------
// 6. Main loop - the frame pipeline
//
//   input:  selectTool -> exportCanvas -> newCanvas -> undoStep -> redoStep
//           -> updateCursor -> handleShapeClicks -> latch release
//           -> paintFreehand -> moveCursorKeys -> typeCharacters -> backspace
//   render: stageTrace (if tracing) -> e.render() (white, full screen)
//           -> paintTrace (grey sparse overlay, if tracing)
// ---------------------------------------------------------------------------

int main() {
    terminalControl t;
    engine e;       // normal engine: the committed canvas, rendered white
    engine top;     // trace layer: ONLY the cells of the shape being traced

    // New engine API: the canvas paints in the default white, so committed
    // art is white in every frame, including while a shape is traced.
    e.setDefaultColor("#FFFFFF");

    if (!t.enableRaw())
        return 1;

    t.enableMouse();
    t.clear();

    // Request normal arrow mouse pointer (OSC 22)
    t.write("\033]22;left_ptr\033\\");

    PaintState state;
    showTool(t, state.tool, state.note);

    while (!t.quit && !t.keys.escape) {
        t.update();

        selectTool(t, state, top);
        exportCanvas(t, state, e);
        newCanvas(t, state, e, top);
        undoStep(t, state, e, top);
        redoStep(t, state, e, top);
        updateCursor(t, state, e);
        handleShapeClicks(t, state, e, top);

        // the commit click ends here - do not smear pencil pixels
        // with the tail of the same button press; button up also closes
        // the current drag as one finished undo step
        if (!t.mouse.left) {
            state.latch = false;
            state.strokeOpen = false;
        }

        paintFreehand(state, e, t);
        moveCursorKeys(t, state, e);
        typeCharacters(t, state, e);

        if (t.keys.backspace) {
            pushUndo(state, e);     // the erased cell stays recoverable
            e.setChar(state.row, state.col, ' ');
        }

        if (state.shaping)
            stageTrace(state, top);

        // Render: the canvas first - full screen, default white, committed
        // art stays white - then the grey trace composited on top of it.
        // On commit / cancel the trace layer is empty, so this same full
        // repaint is what erases the grey preview.
        e.render();

        if (state.shaping)
            paintTrace(t, top);

        // Don't move the terminal's text cursor;
        // it is not the mouse pointer.
    }

    // Restore default mouse pointer
    t.write("\033]22;\033\\");

    t.disableRaw();
    return 0;
}