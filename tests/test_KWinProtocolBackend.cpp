/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/KWinProtocolBackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <gtest/gtest.h>

using TrophyBackend::KWinProtocolBackend;
using TrophyBackend::WindowInfo;

namespace {

void ensureApplication() {
  if (!QCoreApplication::instance()) {
    static int argc = 1;
    static char arg0[] = "test";
    static char *argv[] = {arg0, nullptr};
    new QCoreApplication(argc, argv);
  }
}

void pump(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
}

} // namespace

TEST(KWinProtocolBackendTest, ListsWindowsWithPidAndState) {
  ensureApplication();
  if (!KWinProtocolBackend::isAvailable())
    GTEST_SKIP() << "No Wayland session in the environment";

  KWinProtocolBackend backend;
  QString error;
  if (!backend.start(&error))
    GTEST_SKIP() << error.toStdString();

  // start() already roundtrips the window list; the snapshot is published on
  // the next event-loop turn so a burst of events coalesces.
  pump(300);
  EXPECT_TRUE(backend.isActive());
  EXPECT_TRUE(backend.hasSnapshot());
  EXPECT_GE(backend.protocolVersion(), 17);

  // An empty desktop is a valid result. What must hold is that every row is
  // usable: identity for matching, and a key the monitor can turn into a
  // stable synthetic id.
  for (const WindowInfo &window : backend.windows()) {
    EXPECT_FALSE(window.key.isEmpty()) << "rows need a stable key";
    EXPECT_TRUE(!window.appId.isEmpty() || !window.title.isEmpty())
        << "rows need identity";
    EXPECT_GE(window.width, 0);
    EXPECT_GE(window.height, 0);
  }

  backend.stop();
  EXPECT_FALSE(backend.isActive());
  EXPECT_FALSE(backend.hasSnapshot());
  EXPECT_TRUE(backend.windows().isEmpty());
}

TEST(KWinProtocolBackendTest, RestartAfterStopIsSafe) {
  ensureApplication();
  if (!KWinProtocolBackend::isAvailable())
    GTEST_SKIP() << "No Wayland session in the environment";

  KWinProtocolBackend backend;
  QString error;
  if (!backend.start(&error))
    GTEST_SKIP() << error.toStdString();
  backend.stop();
  ASSERT_TRUE(backend.start(&error)) << error.toStdString();
  pump(200);
  EXPECT_TRUE(backend.isActive());
  backend.stop();
}
