/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WindowMerge.h"

#include <QSet>

#include <algorithm>

namespace TrophyBackend {

namespace {

bool titlesCompatible(const QString &a, const QString &b) {
  return a.isEmpty() || b.isEmpty() || a.compare(b, Qt::CaseInsensitive) == 0;
}

bool appIdMatches(const WindowInfo &a, const WindowInfo &b) {
  return !a.appId.isEmpty() && !b.appId.isEmpty() &&
         a.appId.compare(b.appId, Qt::CaseInsensitive) == 0;
}

bool sameGeometry(const WindowInfo &a, const WindowInfo &b) {
  return a.width > 0 && a.height > 0 && a.width == b.width &&
         a.height == b.height && a.x == b.x && a.y == b.y;
}

bool hasFocus(const QList<WindowInfo> &windows) {
  return std::any_of(windows.cbegin(), windows.cend(),
                     [](const WindowInfo &w) { return w.focused; });
}

} // namespace

QList<WindowInfo> mergeWindowSnapshots(const QList<WindowInfo> &primary,
                                       const QList<WindowInfo> &enrichment) {
  if (enrichment.isEmpty())
    return primary;
  QList<WindowInfo> out = primary;
  const bool primaryHasFocus = hasFocus(primary);
  QSet<int> consumed;
  // Rows that took their data from a focused enrichment window, used to
  // restore focus when the primary list reports none at all.
  QSet<int> focusFromEnrichment;

  for (int row = 0; row < out.size(); ++row) {
    WindowInfo &target = out[row];
    int best = -1;
    for (int i = 0; i < enrichment.size(); ++i) {
      if (consumed.contains(i))
        continue;
      const WindowInfo &candidate = enrichment[i];
      const bool byPid =
          target.pid > 0 && candidate.pid > 0 && target.pid == candidate.pid;
      const bool byApp =
          appIdMatches(target, candidate) &&
          (titlesCompatible(target.title, candidate.title) ||
           sameGeometry(target, candidate));
      if (!byPid && !byApp)
        continue;
      best = i;
      if (byApp)
        break; // an identity match is better than a shared process
    }
    if (best < 0)
      continue;
    const WindowInfo &source = enrichment[best];
    consumed.insert(best);
    if (source.focused)
      focusFromEnrichment.insert(row);
    if (target.pid <= 0)
      target.pid = source.pid;
    if (target.width <= 0 || target.height <= 0) {
      target.x = source.x;
      target.y = source.y;
      target.width = source.width;
      target.height = source.height;
    }
    if (target.outputWidth <= 0 || target.outputHeight <= 0) {
      target.outputWidth = source.outputWidth;
      target.outputHeight = source.outputHeight;
    }
  }

  QSet<QString> keys;
  for (const WindowInfo &w : out)
    keys.insert(w.key);
  for (int i = 0; i < enrichment.size(); ++i) {
    if (consumed.contains(i))
      continue;
    WindowInfo extra = enrichment[i];
    if (!extra.key.isEmpty() && keys.contains(extra.key))
      continue;
    // A single focused window: never let the supplement claim focus when
    // the compositor already named one. Otherwise its focus is the only
    // signal available (X11 _NET_ACTIVE_WINDOW / wlroots activated).
    if (primaryHasFocus)
      extra.focused = false;
    keys.insert(extra.key);
    out.append(extra);
  }

  if (!hasFocus(out) && !focusFromEnrichment.isEmpty()) {
    const int row = *focusFromEnrichment.cbegin();
    if (row >= 0 && row < out.size())
      out[row].focused = true;
  }
  return out;
}

} // namespace TrophyBackend
