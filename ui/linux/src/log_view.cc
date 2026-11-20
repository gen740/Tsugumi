#include "log_view.hh"

#include <array>
#include <utility>

#include "strings.hh"

namespace tsugumi::linux_ui {
namespace D = Tsugumi::Data;

namespace {

// The streams as LogRangeRequest names them, shown by those names as in the
// Web UI.
constexpr std::array kStreams = {
    std::pair{D::LogStream::standard_output, "stdout"},
    std::pair{D::LogStream::standard_error, "stderr"},
    std::pair{D::LogStream::build, "build"},
    std::pair{D::LogStream::journal, "journal"},
};

// GTK shows UTF-8 only; a window may start or end inside a character.
auto Displayable(const std::string& text) -> std::string {
  gchar* valid =
      g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
  std::string out = valid;
  g_free(valid);
  return out;
}

}  // namespace

LogView::LogView()
    : root_(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6)),
      title_(gtk_label_new("")),
      streams_(nullptr),
      earlier_(gtk_button_new_with_label(L("前のログを読む").c_str())),
      follow_(gtk_check_button_new_with_label(L("末尾を追う").c_str())),
      text_view_(gtk_text_view_new()),
      scroll_(gtk_scrolled_window_new()) {
  g_object_ref_sink(root_);
  gtk_widget_set_margin_start(root_, 8);
  gtk_widget_set_margin_end(root_, 8);
  gtk_widget_set_margin_top(root_, 8);
  gtk_widget_set_margin_bottom(root_, 8);

  std::array<const char*, kStreams.size() + 1> names{};
  for (size_t i = 0; i < kStreams.size(); ++i)
    names.at(i) = kStreams.at(i).second;
  streams_ = gtk_drop_down_new_from_strings(names.data());

  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* heading = gtk_label_new(L("ログ").c_str());
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(GTK_BOX(bar), heading);
  gtk_label_set_ellipsize(GTK_LABEL(title_), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(title_, TRUE);
  gtk_label_set_xalign(GTK_LABEL(title_), 0);
  gtk_box_append(GTK_BOX(bar), title_);
  gtk_box_append(GTK_BOX(bar), streams_);
  gtk_box_append(GTK_BOX(bar), earlier_);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(follow_), TRUE);
  gtk_box_append(GTK_BOX(bar), follow_);
  gtk_box_append(GTK_BOX(root_), bar);

  gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view_), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view_), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_), GTK_WRAP_CHAR);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), text_view_);
  gtk_widget_set_vexpand(scroll_, TRUE);
  gtk_box_append(GTK_BOX(root_), scroll_);

  g_signal_connect(streams_, "notify::selected",
                   G_CALLBACK(&LogView::StreamChanged), this);
  g_signal_connect(earlier_, "clicked", G_CALLBACK(&LogView::EarlierClicked),
                   this);
  Reset();
}

LogView::~LogView() {
  g_signal_handlers_disconnect_by_data(streams_, this);
  g_signal_handlers_disconnect_by_data(earlier_, this);
  g_object_unref(root_);
}

auto LogView::stream() const -> D::LogStream {
  const auto index = gtk_drop_down_get_selected(GTK_DROP_DOWN(streams_));
  return index < kStreams.size() ? kStreams.at(index).first
                                 : D::LogStream::standard_output;
}

auto LogView::Show(std::weak_ptr<ProjectSession> session,
                   const std::string& card) -> void {
  session_ = std::move(session);
  card_ = card;
  const auto shown = session_.lock();
  run_ = shown ? shown->run_id() : "";
  Reset();
}

auto LogView::RunChanged() -> void {
  const auto shown = session_.lock();
  const auto run = shown ? shown->run_id() : "";
  if (run == run_) {
    // The run may have ended without a LogAvailable for this Card.
    if (!card_.empty() && !window_.read()) Read(LogWindow::Tail());
    return;
  }
  run_ = run;
  Reset();
}

auto LogView::Reset() -> void {
  ++generation_;
  window_ = LogWindow();
  reading_ = false;
  read_again_ = false;
  const auto shown = session_.lock();
  const auto* card = shown ? shown->scene().Find(card_) : nullptr;
  gtk_label_set_text(GTK_LABEL(title_), card ? card->name.c_str() : "");
  Render();
  if (card && !run_.empty()) Read(LogWindow::Tail());
}

auto LogView::Notice(const LogNotice& notice) -> void {
  if (notice.run_id != run_ || notice.card != card_) return;
  if (notice.end_offset && notice.stream != stream()) return;
  if (notice.end_offset) window_.Grew(*notice.end_offset);
  if (!window_.read() ||
      (window_.behind() &&
       gtk_check_button_get_active(GTK_CHECK_BUTTON(follow_)) != FALSE))
    Read(window_.Next());
}

auto LogView::Read(const LogRequest& request) -> void {
  const auto session = session_.lock();
  if (!session || card_.empty()) return;
  if (reading_) {
    read_again_ = true;
    return;
  }
  reading_ = true;
  const auto generation = generation_;
  session->ReadLog(
      card_, stream(), request,
      [this, alive = std::weak_ptr<int>(alive_), generation,
       read = request.read](std::optional<LogPiece> piece,
                            const std::string& error) {
        // The view may be gone, or show another log, when the reply comes.
        if (alive.expired() || generation != generation_) return;
        reading_ = false;
        if (piece) {
          window_.Apply(read, *piece);
        } else if (!error.empty()) {
          gtk_label_set_text(GTK_LABEL(title_),
                             L("失敗: {0}", {error}).c_str());
        }
        Render();
        const bool follow =
            gtk_check_button_get_active(GTK_CHECK_BUTTON(follow_)) != FALSE;
        if (read_again_ || (piece && follow && window_.behind())) {
          read_again_ = false;
          Read(window_.Next());
        }
      });
}

auto LogView::Render() -> void {
  GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view_));
  std::string shown;
  if (card_.empty())
    shown = L("Card を選ぶと、ここにログを表示します");
  else if (run_.empty())
    shown = L("実行するとここにログが表示されます。");
  else if (window_.read() && window_.text().empty())
    shown = L("この stream への出力はありません");
  else
    shown = Displayable(window_.text());
  gtk_text_buffer_set_text(buffer, shown.c_str(),
                           static_cast<int>(shown.size()));
  gtk_widget_set_sensitive(earlier_, window_.read() && window_.start() > 0);
  if (gtk_check_button_get_active(GTK_CHECK_BUTTON(follow_)) != FALSE) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    GtkTextMark* mark =
        gtk_text_buffer_create_mark(buffer, nullptr, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(text_view_), mark);
    gtk_text_buffer_delete_mark(buffer, mark);
  }
}

auto LogView::StreamChanged(GObject* /*dropdown*/, GParamSpec* /*spec*/,
                            gpointer self) -> void {
  static_cast<LogView*>(self)->Reset();
}

auto LogView::EarlierClicked(GtkButton* /*button*/, gpointer self) -> void {
  auto* view = static_cast<LogView*>(self);
  if (view->window_.start() > 0) view->Read(view->window_.Earlier());
}

}  // namespace tsugumi::linux_ui
