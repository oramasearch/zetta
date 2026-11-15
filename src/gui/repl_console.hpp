// SPDX-License-Identifier: MIT

#pragma once

#include "zetta/repl/console_model.hpp"

// Forward-declared so this header stays out of <imgui.h>; only repl_console.cpp
// (and the App TU that draws it) pulls in the full ImGui definitions.
struct ImFont;

namespace zetta::gui {

// CMP-6 (#524): the Dear ImGui REPL console component. A **thin renderer** over a
// `repl::ConsoleModel` — the model owns the session state (frontend, input,
// transcript) and the submit logic; this only draws the surface and forwards the
// Run click to `model.submit(run)`:
//
//   • a frontend selector (SQL / simulated-Python),
//   • a multiline code editor (the model's input buffer),
//   • Run + Clear controls,
//   • a scrollback transcript of past statements + their results, and
//   • a result grid for the rows a statement returned.
//
// It owns no engine — running a statement is the host's `repl::RunHandler` (the
// "run event" the console emits), so the same component backs the SQL/sim-Python
// Console view today and the governed Console/REPL screen (SCR-8, #525) next.
// The only state held here is view-only (transcript auto-scroll); everything
// durable lives in the model, which a future CLI renders the same way.
class ReplConsole {
public:
    // Render the whole component into the current ImGui window. `run` is the
    // host's run handler; `mono_font` (may be null) renders code + result cells
    // in Geist Mono. The model is mutated in place (input, frontend, transcript).
    void draw(repl::ConsoleModel& model, const repl::RunHandler& run,
              ImFont* mono_font);

private:
    void draw_transcript(const repl::ConsoleModel& model, ImFont* mono_font);
    void draw_result(const repl::Result& result, ImFont* mono_font);
    void draw_result_grid(const repl::Result& result, ImFont* mono_font);

    // Scroll the transcript to the newest entry on the next frame (set after a
    // Run / Clear). View-only — not part of the REPL session state.
    bool scroll_to_bottom_ = false;
};

}  // namespace zetta::gui
