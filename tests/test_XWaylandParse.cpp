/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/XWaylandWindowBackend.h"

#include <gtest/gtest.h>

using TrophyBackend::X11Atoms;
using TrophyBackend::X11WindowRecord;
using TrophyBackend::XWaylandWindowBackend;

namespace {

constexpr quint32 kFullscreen = 0x100;
constexpr quint32 kHidden = 0x200;
constexpr quint32 kSkipTaskbar = 0x400;

X11Atoms atoms() {
  X11Atoms a;
  a.fullscreen = kFullscreen;
  a.hidden = kHidden;
  a.skipTaskbar = kSkipTaskbar;
  return a;
}

X11WindowRecord record(quint32 id, const QString &wmClass, const QString &title,
                       quint32 pid) {
  X11WindowRecord r;
  r.id = id;
  r.wmClass = wmClass;
  r.title = title;
  r.pid = pid;
  r.x = 100;
  r.y = 200;
  r.width = 1280;
  r.height = 720;
  r.viewable = true;
  return r;
}

} // namespace

TEST(XWaylandParseTest, ClassBecomesALowercasedAppId) {
  // Compositors derive app_id from the same WM_CLASS field, so the
  // whitelist keys already used for native windows match here too.
  const auto rows = XWaylandWindowBackend::parseWindows(
      {record(0x400001, "EldenRing", "ELDEN RING", 4242)}, 0, atoms());
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].appId, "eldenring");
  EXPECT_EQ(rows[0].title, "ELDEN RING");
  EXPECT_EQ(rows[0].pid, 4242);
}

TEST(XWaylandParseTest, KeysAreNamespacedAndNumericIdsAreLeftToTheMonitor) {
  const auto rows = XWaylandWindowBackend::parseWindows(
      {record(0x40000a, "game", "Game", 7)}, 0, atoms());
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].key, QStringLiteral("x11:0x40000a"));
  EXPECT_EQ(rows[0].id, 0u);
}

TEST(XWaylandParseTest, TaskbarSkippingAndUnmappedWindowsAreDropped) {
  X11WindowRecord dock = record(0x400002, "plasmashell", "Panel", 1);
  dock.state = {kSkipTaskbar};
  X11WindowRecord hidden = record(0x400003, "game", "Game", 2);
  hidden.viewable = false;
  const auto rows = XWaylandWindowBackend::parseWindows(
      {dock, hidden, record(0x400004, "game2", "Game 2", 3)}, 0, atoms());
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].appId, "game2");
}

TEST(XWaylandParseTest, StateAtomsMapToFullscreenAndMinimized) {
  X11WindowRecord full = record(0x400005, "game", "Game", 3);
  full.state = {kFullscreen};
  X11WindowRecord minimized = record(0x400006, "other", "Other", 4);
  minimized.state = {kHidden};
  const auto rows = XWaylandWindowBackend::parseWindows({full, minimized}, 0,
                                                        atoms());
  ASSERT_EQ(rows.size(), 2);
  EXPECT_TRUE(rows[0].fullscreen);
  EXPECT_FALSE(rows[0].minimized);
  EXPECT_FALSE(rows[1].fullscreen);
  EXPECT_TRUE(rows[1].minimized);
}

TEST(XWaylandParseTest, ActiveWindowMarksExactlyOneRow) {
  const auto rows = XWaylandWindowBackend::parseWindows(
      {record(0x400007, "a", "A", 5), record(0x400008, "b", "B", 6)},
      0x400008, atoms());
  ASSERT_EQ(rows.size(), 2);
  EXPECT_FALSE(rows[0].focused);
  EXPECT_TRUE(rows[1].focused);
}

TEST(XWaylandParseTest, WindowsWithNoPidAreStillListed) {
  // _NET_WM_PID is optional; a window without one is still a window, and
  // detection can fall back to app-id evidence.
  const auto rows = XWaylandWindowBackend::parseWindows(
      {record(0x400009, "game", "Game", 0)}, 0, atoms());
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].pid, 0);
}

TEST(XWaylandParseTest, MissingClassIsDroppedRatherThanAnonymous) {
  const auto rows = XWaylandWindowBackend::parseWindows(
      {record(0x40000b, "", "Untitled", 8)}, 0, atoms());
  EXPECT_TRUE(rows.isEmpty());
}

TEST(XWaylandParseTest, GeometryIsCarriedThrough) {
  const auto rows = XWaylandWindowBackend::parseWindows(
      {record(0x40000c, "game", "Game", 9)}, 0, atoms());
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].x, 100);
  EXPECT_EQ(rows[0].y, 200);
  EXPECT_EQ(rows[0].width, 1280);
  EXPECT_EQ(rows[0].height, 720);
  // No QGuiApplication in this test binary, so no output size is known.
  EXPECT_EQ(rows[0].outputWidth, 0);
}

TEST(XWaylandBackendTest, PollsARealXServerWhenOneIsPresent) {
  if (!XWaylandWindowBackend::isAvailable())
    GTEST_SKIP() << "No DISPLAY in the environment";
  XWaylandWindowBackend backend;
  if (!backend.start())
    GTEST_SKIP() << "No usable X server with _NET_CLIENT_LIST";
  backend.refresh();
  for (const auto &window : backend.windows()) {
    fprintf(stderr, "x11 window app_id=%s pid=%lld %dx%d focus=%d full=%d\n",
            qUtf8Printable(window.appId), (long long)window.pid, window.width,
            window.height, int(window.focused), int(window.fullscreen));
    EXPECT_FALSE(window.appId.isEmpty());
    EXPECT_FALSE(window.key.isEmpty());
  }
  EXPECT_TRUE(backend.isActive());
  backend.stop();
  EXPECT_TRUE(backend.windows().isEmpty());
}
