#include "main_thread.hh"

#include <glib.h>

#include <utility>

namespace tsugumi::linux_ui {

auto Post(std::function<void()> task) -> void {
  auto* owned = new std::function<void()>(std::move(task));
  g_main_context_invoke_full(
      nullptr, G_PRIORITY_DEFAULT,
      [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()>*>(data))();
        return G_SOURCE_REMOVE;
      },
      owned,
      [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

}  // namespace tsugumi::linux_ui
