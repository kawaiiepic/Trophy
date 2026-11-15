/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "WindowInfo.h"

#include <QHash>
#include <QList>
#include <QSize>
#include <QString>

#include <functional>

namespace TrophyBackend {

/**
 * @brief Event-ordered window state for org_kde_plasma_window_management.
 *
 * The protocol streams independent events per window (app_id, title, pid,
 * state, geometry) and marks the initial batch with `initial_state`. This
 * model owns that ordering, the uuid deduplication and the translation to
 * WindowInfo, so the Wayland adapter stays a thin mapping and the tricky
 * cases are unit-testable without a compositor.
 *
 * Two protocol quirks are handled here rather than in the adapter:
 *  - a window can be announced twice (the stacking-order reply plus a
 *    `window_with_uuid` event), so records are keyed by uuid;
 *  - `unmapped` means the window closed, and must not leave a ghost row
 *    in detection, the picker or capture restoration.
 */
class PlasmaToplevelModel {
public:
  /** Frame geometry in logical coordinates -> size of the output holding it.
   *  Optional: a compositor protocol gives window geometry, not the layout of
   *  the outputs, and detection treats a missing output size as "unknown". */
  using OutputSizeFn = std::function<QSize(int x, int y)>;

  enum State : quint32 {
    StateActive = 0x1,
    StateMinimized = 0x2,
    StateMaximized = 0x4,
    StateFullscreen = 0x8,
  };

  /** Register a window; a repeated uuid reuses its record. 0 = ignored. */
  quintptr ensure(const QString &uuid);

  bool contains(quintptr handle) const;
  int size() const;
  QString uuid(quintptr handle) const;

  void setAppId(quintptr handle, const QString &appId);
  /** X11 WM_CLASS, used for XWayland windows that report no app_id. */
  void setResourceName(quintptr handle, const QString &resourceName);
  void setTitle(quintptr handle, const QString &title);
  void setPid(quintptr handle, qint64 pid);
  void setState(quintptr handle, quint32 flags);
  /** Frame geometry (includes decorations). */
  void setGeometry(quintptr handle, int x, int y, int width, int height);
  /** Client area; preferred over the frame geometry when present. */
  void setClientGeometry(quintptr handle, int x, int y, int width, int height);

  /** The window's initial batch is complete (`initial_state`/`mapped`). */
  void announce(quintptr handle);
  /** Forget the window entirely (closed or connection teardown). */
  void remove(quintptr handle);

  /** Announced, mapped windows sorted by uuid for a stable snapshot order. */
  QList<WindowInfo> snapshot(const OutputSizeFn &outputSize = {}) const;

private:
  struct Record {
    QString uuid;
    QString appId;
    QString resourceName;
    QString title;
    qint64 pid = 0;
    quint32 state = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool haveClientGeometry = false;
    bool announced = false;
  };

  QHash<quintptr, Record> m_records;
  QHash<QString, quintptr> m_byUuid;
  quintptr m_next = 1;
};

} // namespace TrophyBackend
