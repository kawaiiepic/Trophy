/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PlasmaToplevelModel.h"

#include <algorithm>

namespace TrophyBackend {

quintptr PlasmaToplevelModel::ensure(const QString &uuid) {
  if (uuid.isEmpty())
    return 0;
  const auto existing = m_byUuid.constFind(uuid);
  if (existing != m_byUuid.constEnd())
    return existing.value();
  const quintptr handle = m_next++;
  Record record;
  record.uuid = uuid;
  m_records.insert(handle, record);
  m_byUuid.insert(uuid, handle);
  return handle;
}

bool PlasmaToplevelModel::contains(quintptr handle) const {
  return m_records.contains(handle);
}

int PlasmaToplevelModel::size() const { return m_records.size(); }

QString PlasmaToplevelModel::uuid(quintptr handle) const {
  return m_records.value(handle).uuid;
}

void PlasmaToplevelModel::setAppId(quintptr handle, const QString &appId) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->appId = appId;
}

void PlasmaToplevelModel::setResourceName(quintptr handle,
                                         const QString &resourceName) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->resourceName = resourceName;
}

void PlasmaToplevelModel::setTitle(quintptr handle, const QString &title) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->title = title;
}

void PlasmaToplevelModel::setPid(quintptr handle, qint64 pid) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->pid = pid;
}

void PlasmaToplevelModel::setState(quintptr handle, quint32 flags) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->state = flags;
}

void PlasmaToplevelModel::setGeometry(quintptr handle, int x, int y, int width,
                                      int height) {
  const auto it = m_records.find(handle);
  if (it == m_records.end() || it->haveClientGeometry)
    return;
  it->x = x;
  it->y = y;
  it->width = width;
  it->height = height;
}

void PlasmaToplevelModel::setClientGeometry(quintptr handle, int x, int y,
                                            int width, int height) {
  const auto it = m_records.find(handle);
  if (it == m_records.end())
    return;
  it->x = x;
  it->y = y;
  it->width = width;
  it->height = height;
  it->haveClientGeometry = true;
}

void PlasmaToplevelModel::announce(quintptr handle) {
  const auto it = m_records.find(handle);
  if (it != m_records.end())
    it->announced = true;
}

void PlasmaToplevelModel::remove(quintptr handle) {
  const auto it = m_records.find(handle);
  if (it == m_records.end())
    return;
  m_byUuid.remove(it->uuid);
  m_records.erase(it);
}

QList<WindowInfo> PlasmaToplevelModel::snapshot(
    const OutputSizeFn &outputSize) const {
  QList<WindowInfo> rows;
  rows.reserve(m_records.size());
  for (const Record &record : m_records) {
    if (!record.announced)
      continue;
    WindowInfo info;
    // The compositor protocol has no numeric window id. The monitor turns
    // the stable key into a synthetic id, so rows keep tracking across
    // snapshots; `key` is the same uuid the KWin script reports as
    // internalId, which keeps state stable if the backend changes.
    info.key = record.uuid;
    info.appId = record.appId.isEmpty() ? record.resourceName : record.appId;
    info.title = record.title;
    info.pid = record.pid;
    info.fullscreen = (record.state & StateFullscreen) != 0;
    info.focused = (record.state & StateActive) != 0;
    info.minimized = (record.state & StateMinimized) != 0;
    info.x = record.x;
    info.y = record.y;
    info.width = record.width;
    info.height = record.height;
    if (outputSize) {
      const QSize size = outputSize(record.x, record.y);
      if (size.isValid() && size.width() > 0 && size.height() > 0) {
        info.outputWidth = size.width();
        info.outputHeight = size.height();
      }
    }
    rows.append(info);
  }
  std::sort(rows.begin(), rows.end(),
            [](const WindowInfo &a, const WindowInfo &b) { return a.key < b.key; });
  return rows;
}

} // namespace TrophyBackend
