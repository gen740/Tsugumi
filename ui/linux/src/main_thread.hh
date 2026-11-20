// GTK runs on the main thread; RPC replies and events arrive on the client's
// I/O thread and are handed over here.
#pragma once

#include <functional>

namespace tsugumi::linux_ui {

// Runs task on the GLib default main context, from any thread.
auto Post(std::function<void()> task) -> void;

}  // namespace tsugumi::linux_ui
