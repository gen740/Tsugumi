// Terminal text in failures: what a child wrote for a terminal reaches people
// as plain text.
#include <gtest/gtest.h>

#include <string>

#include "tsugumi/util/text.hh"
#include "tsugumi/workerd/values.hh"

namespace tsugumi {
namespace {

using text::strip_terminal_controls;

TEST(StripTerminalControls, RemovesColours) {
  // Nix's own error, as a remote worker reported it.
  EXPECT_EQ(strip_terminal_controls(
                "\x1b[31;1merror:\x1b[0m unable to download "
                "'\x1b[35;1mhttps://cache.example/x.narinfo\x1b[0m': HTTP "
                "error \x1b[35;1m502\x1b[0m\x1b[35;1m\x1b[0m"),
            "error: unable to download 'https://cache.example/x.narinfo': "
            "HTTP error 502");
  EXPECT_EQ(strip_terminal_controls("\x1b[m\x1b[38;5;196mred\x1b[39m"), "red");
}

TEST(StripTerminalControls, RemovesOsc8Links) {
  EXPECT_EQ(strip_terminal_controls(
                "see \x1b]8;;https://example.com/a\x1b\\the docs\x1b]8;;\x1b\\"),
            "see the docs");
  // BEL also ends an OSC (a window title here).
  EXPECT_EQ(strip_terminal_controls("\x1b]0;title\x07text"), "text");
  // An unterminated string control ends the text.
  EXPECT_EQ(strip_terminal_controls("before\x1b]8;;https://cut"), "before");
}

TEST(StripTerminalControls, RemovesCursorMovesAndOtherControls) {
  EXPECT_EQ(strip_terminal_controls("50%\x1b[2K\x1b[1G\r100%\x1b[?25h"),
            "50%100%");
  EXPECT_EQ(strip_terminal_controls("\x1b(Bplain\x1b=\x1b" "7x\x1b" "8"),
            "plainx");
  EXPECT_EQ(strip_terminal_controls(
                std::string_view("a\x07" "b\x08" "c\x7f" "d\0" "e", 9)),
            "abcde");
  EXPECT_EQ(strip_terminal_controls("line one\n\tline two\r\n"),
            "line one\n\tline two\n");
  // C1 forms: CSI (U+009B) and OSC (U+009D) ... ST (U+009C).
  EXPECT_EQ(strip_terminal_controls("\xc2\x9b" "1mbold \xc2\x9d" "0;t\xc2\x9c" "end"),
            "bold end");
}

TEST(StripTerminalControls, DropsALoneEsc) {
  EXPECT_EQ(strip_terminal_controls("cut\x1b"), "cut");
  EXPECT_EQ(strip_terminal_controls("a\x1b\nb"), "a\nb");
  // A malformed CSI ends at the first byte that cannot belong to it.
  EXPECT_EQ(strip_terminal_controls("\x1b[12\xe3\x81\x82"), "\xe3\x81\x82");
}

TEST(StripTerminalControls, KeepsMultibyteText) {
  const std::string japanese = "検証に失敗しました: キャッシュ 🐦 é";
  EXPECT_EQ(strip_terminal_controls(japanese), japanese);
  EXPECT_EQ(strip_terminal_controls("\x1b[1m" + japanese + "\x1b[0m"), japanese);
  // U+00A0 and U+00C2 share the lead byte with the C1 controls.
  EXPECT_EQ(strip_terminal_controls("\xc2\xa0\xc3\x82"), "\xc2\xa0\xc3\x82");
}

TEST(FailureDiagnostic, KeepsOnlyText) {
  const auto failure = FailureData::make(
      FailureKind::nix_failed, "\x1b[31;1merror:\x1b[0m HTTP error 502");
  ASSERT_TRUE(failure.diagnostic);
  EXPECT_EQ(*failure.diagnostic, "error: HTTP error 502");
  EXPECT_FALSE(FailureData::make(FailureKind::nix_failed, "\x1b[0m").diagnostic);
  // Stored failures read back clean, whoever wrote them.
  FailureData stored;
  stored.kind = FailureKind::nix_failed;
  stored.diagnostic = "\x1b[31merror:\x1b[0m old record";
  const auto read = FailureData::from_bytes(stored.to_bytes());
  ASSERT_TRUE(read && read->diagnostic);
  EXPECT_EQ(*read->diagnostic, "error: old record");
}

}  // namespace
}  // namespace tsugumi
