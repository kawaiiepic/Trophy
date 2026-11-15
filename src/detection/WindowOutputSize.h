/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QPoint>
#include <QSize>

namespace TrophyBackend {

/**
 * @brief Logical size of the output containing a point, when Qt knows it.
 *
 * Wayland window protocols give window geometry but not the output layout,
 * so borderless-coverage scoring needs this. Zero = unknown, which callers
 * treat as "rely on the fullscreen flag".
 */
QSize outputSizeAtPoint(int x, int y);

} // namespace TrophyBackend
