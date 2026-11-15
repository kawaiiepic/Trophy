/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WlrToplevelModel.h"

#include <algorithm>

namespace TrophyBackend {

quintptr WlrToplevelModel::add() {
  const quintptr handle = m_next++;
  Record record;
  record.handle = handle;
  m_records.insert(handle, record);
  return handle;
}

bool WlrToplevelModel::contains(quintptr handle) const {
  return m_records.contains(handle);
}

int WlrToplevelModel::size() const { return m_records.size(); }

void WlrToplevelModel::setAppId(quintptr handle, const QString &appId) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->appId = appId;
}

void WlrToplevelModel::setTitle(quintptr handle, const QString &title) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->title = title;
}

void WlrToplevelModel::setStateIndices(quintptr handle,
                                       const QList<quint32> &indices) {
  const auto it = m_records.find(handle);
  if (it == m_records.end())
    return;
  quint32 state = 0;
  for (const quint32 index : indices) {
    if (index <= StateFullscreen)
      state |= (1u << index);
  }
  it->state = state;
}

void WlrToplevelModel::commit(quintptr handle) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->announced = true;
}

void WlrToplevelModel::remove(quintptr handle) { m_records.remove(handle); }

QList<WindowInfo> WlrToplevelModel::snapshot() const {
  QList<WindowInfo> rows;
  rows.reserve(m_records.size());
  for (const Record &record : m_records) {
    if (!record.announced)
      continue;
    WindowInfo info;
    // No numeric compositor id: the key gives the monitor a stable
    // synthetic id, so rows keep tracking across snapshots.
    info.key = QStringLiteral("wlr:%1").arg(record.handle);
    info.appId = record.appId;
    info.title = record.title;
    info.pid = 0; // the protocol has no process id; enrichment supplies it
    info.focused = (record.state & (1u << StateActivated)) != 0;
    info.fullscreen = (record.state & (1u << StateFullscreen)) != 0;
    info.minimized = (record.state & (1u << StateMinimized)) != 0;
    rows.append(info);
  }
  std::sort(rows.begin(), rows.end(),
            [](const WindowInfo &a, const WindowInfo &b) { return a.key < b.key; });
  return rows;
}

} // namespace TrophyBackend
