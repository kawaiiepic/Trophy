/*
 * Trophy - Game recording and clipping
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "detection/UmbrielBackend.h"
#include <gtest/gtest.h>
using TrophyBackend::UmbrielBackend;

TEST(UmbrielBackendTest, OpaqueIdsFocusPidAndGeometry) {
  const QByteArray windows = R"json([
    {"id":"uuid-a","app_id":"steam_app_489830","title":"Skyrim",
     "pid":4210,"active":false,"focused":true,"fullscreen":true,
     "minimized":false,"x":10,"y":20,"w":1920,"h":1080,"output":"DP-1"},
    {"id":"uuid-b","app_id":"kitty","title":"Terminal",
     "pid":-1,"active":true,"fullscreen":false,"minimized":true,
     "x":0,"y":0,"w":900,"h":500,"output":"eDP-1"}
  ])json";
  const QByteArray focus = R"json({"id":"uuid-a","app_id":"steam_app_489830"})json";
  const QByteArray outputs = R"json([
    {"name":"DP-1","logical_size":{"width":1920,"height":1080}},
    {"name":"eDP-1","logical_size":{"width":1536,"height":960}}
  ])json";
  const auto result = UmbrielBackend::parseWindows(windows, focus, outputs);
  ASSERT_EQ(result.size(), 2);
  const auto &game = result[0];
  const auto &terminal = result[1];
  EXPECT_EQ(game.key, QStringLiteral("umbriel:uuid-a"));
  EXPECT_EQ(game.pid, 4210);
  EXPECT_TRUE(game.focused);
  EXPECT_TRUE(game.fullscreen);
  EXPECT_EQ(game.width, 1920);
  EXPECT_EQ(game.outputWidth, 1920);
  EXPECT_EQ(game.outputHeight, 1080);
  EXPECT_EQ(terminal.pid, 0);
  EXPECT_FALSE(terminal.focused);
  EXPECT_TRUE(terminal.minimized);
}
TEST(UmbrielBackendTest, FallsBackToActiveWithoutFocusedWindow) {
  auto rows = UmbrielBackend::parseWindows(
      R"json([{"id":"opaque-2","app_id":"game","active":true,"pid":22,"w":800,"h":600}])json");
  ASSERT_EQ(rows.size(), 1);
  EXPECT_TRUE(rows[0].focused);
  EXPECT_EQ(rows[0].key, QStringLiteral("umbriel:opaque-2"));
  EXPECT_EQ(rows[0].outputWidth, 0);
}
TEST(UmbrielBackendTest, InvalidSnapshotsAndMissingIds) {
  EXPECT_TRUE(UmbrielBackend::parseWindows("{}").isEmpty());
  EXPECT_TRUE(UmbrielBackend::parseWindows("not json").isEmpty());
  EXPECT_TRUE(UmbrielBackend::parseWindows(R"json([{"title":"No id"}])json").isEmpty());
  EXPECT_TRUE(UmbrielBackend::parseWindows("[]").isEmpty());
}
TEST(UmbrielBackendTest, ChangeSignatureTracksFocusAndFullscreen) {
  auto a = UmbrielBackend::parseWindows(R"json([{"id":"1","active":false}])json");
  auto b = UmbrielBackend::parseWindows(R"json([{"id":"1","active":true}])json");
  EXPECT_NE(UmbrielBackend::signatureFor(a), UmbrielBackend::signatureFor(b));
  auto c = UmbrielBackend::parseWindows(R"json([{"id":"1","active":true,"fullscreen":true}])json");
  EXPECT_NE(UmbrielBackend::signatureFor(b), UmbrielBackend::signatureFor(c));
}

TEST(UmbrielBackendTest, UnwrapsRawSocketArrayAndObjectReplies) {
  QString error;
  const QByteArray windows = UmbrielBackend::unwrapReply(
      R"json({"ok":[{"id":"opaque","pid":99,"active":true}]})json", &error);
  EXPECT_TRUE(error.isEmpty());
  const auto rows = UmbrielBackend::parseWindows(windows);
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].pid, 99);
  EXPECT_TRUE(rows[0].focused);

  const QByteArray focused = UmbrielBackend::unwrapReply(
      R"json({"ok":{"id":"opaque","app_id":"game"}})json", &error);
  EXPECT_TRUE(error.isEmpty());
  EXPECT_FALSE(focused.isEmpty());
  const auto withFocus = UmbrielBackend::parseWindows(windows, focused);
  ASSERT_EQ(withFocus.size(), 1);
  EXPECT_TRUE(withFocus[0].focused);
}

TEST(UmbrielBackendTest, DetectsSocketErrorsAndMalformedReplies) {
  QString error;
  EXPECT_TRUE(UmbrielBackend::unwrapReply(
      R"json({"err":"no focused window"})json", &error).isEmpty());
  EXPECT_EQ(error, QStringLiteral("no focused window"));
  error.clear();
  EXPECT_TRUE(UmbrielBackend::unwrapReply("not json", &error).isEmpty());
  EXPECT_EQ(error, QStringLiteral("invalid JSON reply"));
  error.clear();
  EXPECT_TRUE(UmbrielBackend::unwrapReply(R"json({"oops":1})json", &error).isEmpty());
  EXPECT_EQ(error, QStringLiteral("reply missing 'ok'"));
}
