/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WindowOutputSize.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>

namespace TrophyBackend {

QSize outputSizeAtPoint(int x, int y) {
  // Tests use QCoreApplication (no screens at all); never assert on that.
  if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
    return {};
  const QPoint point(x, y);
  const auto screens = QGuiApplication::screens();
  for (QScreen *screen : screens) {
    if (screen && screen->geometry().contains(point))
      return screen->geometry().size();
  }
  return {};
}

} // namespace TrophyBackend
