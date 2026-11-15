/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/WlrToplevelBackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <gtest/gtest.h>

using TrophyBackend::WindowInfo;
using TrophyBackend::WlrToplevelBackend;

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

TEST(WlrToplevelBackendTest, ListsToplevelsOnACompositorThatSupportsIt) {
  ensureApplication();
  if (!WlrToplevelBackend::isAvailable())
    GTEST_SKIP() << "No Wayland session in the environment";

  WlrToplevelBackend backend;
  QString error;
  if (!backend.start(&error))
    GTEST_SKIP() << error.toStdString();

  pump(300);
  EXPECT_TRUE(backend.isActive());
  EXPECT_TRUE(backend.hasSnapshot());
  for (const WindowInfo &window : backend.windows()) {
    EXPECT_FALSE(window.key.isEmpty());
    EXPECT_GE(window.width, 0);
  }

  backend.stop();
  EXPECT_FALSE(backend.isActive());
  EXPECT_TRUE(backend.windows().isEmpty());
}

TEST(WlrToplevelBackendTest, RestartAfterStopIsSafe) {
  ensureApplication();
  if (!WlrToplevelBackend::isAvailable())
    GTEST_SKIP() << "No Wayland session in the environment";

  WlrToplevelBackend backend;
  QString error;
  if (!backend.start(&error))
    GTEST_SKIP() << error.toStdString();
  backend.stop();
  ASSERT_TRUE(backend.start(&error)) << error.toStdString();
  pump(200);
  EXPECT_TRUE(backend.isActive());
  backend.stop();
}
