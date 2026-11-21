#include "project_view.hh"

#include <utility>

#include "strings.hh"

namespace tsugumi::linux_ui {

namespace {

auto Button(const std::string& label) -> GtkWidget* {
  return gtk_button_new_with_label(label.c_str());
}

}  // namespace

ProjectView::ProjectView()
    : root_(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0)),
      home_(Button(L("Home へ戻る"))),
      title_(gtk_label_new("")),
      status_(gtk_label_new("")),
      run_(Button(L("ここから下流を実行"))),
      stop_(Button(L("実行を停止"))),
      pages_(gtk_stack_new()),
      spinner_(gtk_spinner_new()),
      message_(gtk_label_new("")),
      detail_(gtk_label_new("")) {
  g_object_ref_sink(root_);
  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(bar, 8);
  gtk_widget_set_margin_end(bar, 8);
  gtk_widget_set_margin_top(bar, 8);
  gtk_widget_set_margin_bottom(bar, 8);
  gtk_box_append(GTK_BOX(bar), home_);
  gtk_widget_add_css_class(title_, "heading");
  gtk_label_set_ellipsize(GTK_LABEL(title_), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(bar), title_);
  gtk_widget_set_hexpand(status_, TRUE);
  gtk_label_set_xalign(GTK_LABEL(status_), 1);
  gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(bar), status_);
  gtk_widget_add_css_class(run_, "suggested-action");
  gtk_box_append(GTK_BOX(bar), run_);
  gtk_box_append(GTK_BOX(bar), stop_);
  gtk_box_append(GTK_BOX(root_), bar);
  gtk_box_append(GTK_BOX(root_), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  // Starting or failed.
  GtkWidget* waiting = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(waiting, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(waiting, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(waiting), spinner_);
  gtk_widget_add_css_class(message_, "title-3");
  gtk_label_set_wrap(GTK_LABEL(message_), TRUE);
  gtk_box_append(GTK_BOX(waiting), message_);
  gtk_label_set_wrap(GTK_LABEL(detail_), TRUE);
  gtk_label_set_selectable(GTK_LABEL(detail_), TRUE);
  gtk_widget_add_css_class(detail_, "dim-label");
  gtk_box_append(GTK_BOX(waiting), detail_);
  gtk_stack_add_named(GTK_STACK(pages_), waiting, "waiting");

  GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child(GTK_PANED(paned), canvas_.widget());
  gtk_paned_set_end_child(GTK_PANED(paned), log_.widget());
  gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
  gtk_widget_set_size_request(log_.widget(), 360, -1);
  gtk_paned_set_position(GTK_PANED(paned), 720);
  gtk_stack_add_named(GTK_STACK(pages_), paned, "canvas");
  gtk_widget_set_vexpand(pages_, TRUE);
  gtk_box_append(GTK_BOX(root_), pages_);

  g_signal_connect(home_, "clicked", G_CALLBACK(&ProjectView::HomeClicked),
                   this);
  g_signal_connect(run_, "clicked", G_CALLBACK(&ProjectView::RunClicked), this);
  g_signal_connect(stop_, "clicked", G_CALLBACK(&ProjectView::StopClicked),
                   this);
  canvas_.on_select = [this](const std::string& card) {
    log_.Show(session_, card);
    Update();
  };
  Update();
}

ProjectView::~ProjectView() {
  if (session_) {
    session_->on_changed = nullptr;
    session_->on_log = nullptr;
  }
  for (auto* button : {home_, run_, stop_})
    g_signal_handlers_disconnect_by_data(button, this);
  g_object_unref(root_);
}

auto ProjectView::Show(std::shared_ptr<ProjectSession> session) -> void {
  if (session_) {
    session_->on_changed = nullptr;
    session_->on_log = nullptr;
  }
  session_ = std::move(session);
  reveal_.clear();
  canvas_.Select("");
  shown_run_.clear();
  log_.Show(session_, "");
  if (session_) {
    session_->on_changed = [this] { Update(); };
    session_->on_log = [this](const LogNotice& notice) { log_.Notice(notice); };
  }
  Update();
}

auto ProjectView::Reveal(const std::string& card) -> void {
  reveal_ = card;
  Update();
}

auto ProjectView::Update() -> void {
  const auto* session = session_.get();
  const bool open = session && session->phase() == ProjectSession::Phase::kOpen;
  gtk_label_set_text(GTK_LABEL(title_),
                     session ? session->title().c_str() : "");
  gtk_stack_set_visible_child_name(GTK_STACK(pages_),
                                   open ? "canvas" : "waiting");
  const bool starting =
      session && session->phase() == ProjectSession::Phase::kStarting;
  gtk_widget_set_visible(spinner_, starting ? TRUE : FALSE);
  gtk_spinner_set_spinning(GTK_SPINNER(spinner_), starting ? TRUE : FALSE);
  gtk_label_set_text(GTK_LABEL(message_),
                     session ? session->message().c_str() : "");
  gtk_label_set_text(GTK_LABEL(detail_),
                     session ? session->detail().c_str() : "");
  if (!open) {
    gtk_label_set_text(GTK_LABEL(status_), "");
    gtk_widget_set_sensitive(run_, FALSE);
    gtk_widget_set_sensitive(stop_, FALSE);
    canvas_.Show(nullptr, nullptr);
    if (on_changed) on_changed();
    return;
  }
  canvas_.Show(&session->scene(), [session](const std::string& card) {
    return session->CardState(card);
  });
  if (!reveal_.empty() && session->scene().Find(reveal_)) {
    canvas_.Select(reveal_);
    log_.Show(session_, reveal_);
    reveal_.clear();
  }
  if (!canvas_.selected().empty() &&
      !session->scene().Find(canvas_.selected())) {
    canvas_.Select("");
    log_.Show(session_, "");
  }
  std::string status = session->message();
  if (status.empty() && !session->run_id().empty()) {
    const auto* state = RunStateText(session->run_state());
    if (*state != '\0') status = L("実行: {0}", {L(state)});
  }
  gtk_label_set_text(GTK_LABEL(status_), status.c_str());
  const auto* selected = session->scene().Find(canvas_.selected());
  gtk_widget_set_sensitive(
      run_, selected && !selected->definition_error && !session->running());
  gtk_widget_set_sensitive(stop_, session->running());
  if (shown_run_ != session->run_id() || !session->running()) {
    shown_run_ = session->run_id();
    log_.RunChanged();
  }
  if (on_changed) on_changed();
}

auto ProjectView::HomeClicked(GtkButton* /*button*/, gpointer self) -> void {
  auto* view = static_cast<ProjectView*>(self);
  if (view->on_home) view->on_home();
}

auto ProjectView::RunClicked(GtkButton* /*button*/, gpointer self) -> void {
  auto* view = static_cast<ProjectView*>(self);
  if (!view->session_) return;
  const auto* card = view->session_->scene().Find(view->canvas_.selected());
  if (card && !card->definition_error) view->session_->Run(card->id);
}

auto ProjectView::StopClicked(GtkButton* /*button*/, gpointer self) -> void {
  auto* view = static_cast<ProjectView*>(self);
  if (view->session_) view->session_->Cancel();
}

}  // namespace tsugumi::linux_ui
