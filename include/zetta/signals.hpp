#pragma once

namespace zetta {

// Install SIGINT + SIGTERM handlers that set a process-global "stop requested"
// flag. The handler does nothing but an atomic store, so it's async-signal-safe.
// Idempotent; call once per run mode (the headless loop and the headed UI both
// use it so terminal Ctrl-C quits either one).
void install_signal_handlers();

// Whether SIGINT/SIGTERM has been received since the handlers were installed.
bool stop_requested();

}  // namespace zetta
