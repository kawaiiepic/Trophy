/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "WindowInfo.h"

#include <QList>

namespace TrophyBackend {

/**
 * @brief Combine a compositor snapshot with a supplementary one.
 *
 * Wayland listing protocols give identity and window state but no PID;
 * XWayland gives PID, geometry and output size. Merging keeps one row per
 * window: the primary row keeps its id/key (so monitor state is stable) and
 * only gains the fields it is missing.
 *
 * Matching is by non-empty app_id (case-insensitive) with compatible
 * titles, or by PID when both sides know it. `primary` wins on conflicts;
 * enrichment rows unknown to the primary are appended so a compositor with
 * no window backend still sees its XWayland games.
 */
QList<WindowInfo> mergeWindowSnapshots(const QList<WindowInfo> &primary,
                                       const QList<WindowInfo> &enrichment);

} // namespace TrophyBackend
