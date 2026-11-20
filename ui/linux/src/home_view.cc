#include "home_view.hh"

#include <utility>

#include "strings.hh"

namespace tsugumi::linux_ui {

HomeView::HomeView()
    : root_(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12)),
      list_(gtk_list_box_new()),
      error_(gtk_label_new("")),
      button_(gtk_button_new_with_label(L("プロジェクトを開く…").c_str())),
      choosing_(g_cancellable_new()) {
  g_object_ref_sink(root_);
  gtk_widget_set_margin_top(root_, 24);
  gtk_widget_set_margin_bottom(root_, 24);
  gtk_widget_set_margin_start(root_, 24);
  gtk_widget_set_margin_end(root_, 24);

  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget* title = gtk_label_new(L("最近のプロジェクト").c_str());
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_box_append(GTK_BOX(header), title);
  gtk_widget_add_css_class(button_, "suggested-action");
  g_signal_connect(button_, "clicked", G_CALLBACK(&HomeView::Choose), this);
  gtk_box_append(GTK_BOX(header), button_);
  gtk_box_append(GTK_BOX(root_), header);

  gtk_label_set_wrap(GTK_LABEL(error_), TRUE);
  gtk_label_set_xalign(GTK_LABEL(error_), 0);
  gtk_widget_add_css_class(error_, "error");
  gtk_widget_set_visible(error_, FALSE);
  gtk_box_append(GTK_BOX(root_), error_);

  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list_), TRUE);
  gtk_widget_add_css_class(list_, "boxed-list");
  GtkWidget* empty =
      gtk_label_new(L("最近開いたプロジェクトはありません").c_str());
  gtk_widget_add_css_class(empty, "dim-label");
  gtk_widget_set_margin_top(empty, 24);
  gtk_widget_set_margin_bottom(empty, 24);
  gtk_list_box_set_placeholder(GTK_LIST_BOX(list_), empty);
  g_signal_connect(list_, "row-activated", G_CALLBACK(&HomeView::Activated),
                   this);
  GtkWidget* scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_);
  gtk_box_append(GTK_BOX(root_), scroll);
}

HomeView::~HomeView() {
  g_cancellable_cancel(choosing_);
  g_object_unref(choosing_);
  g_signal_handlers_disconnect_by_data(button_, this);
  g_signal_handlers_disconnect_by_data(list_, this);
  g_object_unref(root_);
}

auto HomeView::ShowRecent(std::vector<RecentEntry> recent) -> void {
  recent_ = std::move(recent);
  gtk_list_box_remove_all(GTK_LIST_BOX(list_));
  for (const auto& entry : recent_) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    GtkWidget* name = gtk_label_new(entry.name.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_widget_add_css_class(name, "heading");
    GtkWidget* path = gtk_label_new(entry.path.c_str());
    gtk_label_set_xalign(GTK_LABEL(path), 0);
    gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(path, "dim-label");
    gtk_box_append(GTK_BOX(row), name);
    gtk_box_append(GTK_BOX(row), path);
    gtk_list_box_append(GTK_LIST_BOX(list_), row);
  }
}

auto HomeView::ShowError(const std::string& message) -> void {
  gtk_label_set_text(GTK_LABEL(error_), message.c_str());
  gtk_widget_set_visible(error_, message.empty() ? FALSE : TRUE);
}

auto HomeView::Activated(GtkListBox* /*list*/, GtkListBoxRow* row,
                         gpointer self) -> void {
  auto* view = static_cast<HomeView*>(self);
  const auto index = gtk_list_box_row_get_index(row);
  if (index < 0 || static_cast<size_t>(index) >= view->recent_.size()) return;
  if (view->on_open) view->on_open(view->recent_[index].path);
}

auto HomeView::Choose(GtkButton* button, gpointer self) -> void {
  auto* view = static_cast<HomeView*>(self);
  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(
      dialog, L("tsugumi.json を含むフォルダを選んでください").c_str());
  gtk_file_dialog_set_modal(dialog, TRUE);
  auto* window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
  gtk_file_dialog_select_folder(dialog, window, view->choosing_,
                                &HomeView::Chosen, view);
  g_object_unref(dialog);
}

auto HomeView::Chosen(GObject* dialog, GAsyncResult* result, gpointer self)
    -> void {
  GError* error = nullptr;
  GFile* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(dialog),
                                                       result, &error);
  if (!folder) {
    // Cancelled by the person or because Home went away: nothing to do.
    g_clear_error(&error);
    return;
  }
  char* path = g_file_get_path(folder);
  g_object_unref(folder);
  auto* view = static_cast<HomeView*>(self);
  if (path && view->on_open) view->on_open(path);
  g_free(path);
}

}  // namespace tsugumi::linux_ui
