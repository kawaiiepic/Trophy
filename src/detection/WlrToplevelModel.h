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
#include <QString>

namespace TrophyBackend {

/**
 * @brief Event-ordered state for wlr-foreign-toplevel-management.
 *
 * The wlroots family (Sway, Hyprland, labwc, river, Wayfire, niri, …)
 * exposes the same toplevel list to any client: app_id, title and a state
 * bitfield with activated/fullscreen/minimized. There is no PID and no
 * geometry, so rows from here either get those from an enrichment source or
 * stay PID-less and detect on identity evidence alone.
 *
 * Events stream per window and are committed atomically by `done`; the model
 * owns that batching plus `closed`, so the adapter stays thin.
 */
class WlrToplevelModel {
public:
  enum State : quint32 {
    StateMaximized = 0,
    StateMinimized = 1,
    StateActivated = 2,
    StateFullscreen = 3,
  };

  /** Register a new toplevel handle. */
  quintptr add();
  bool contains(quintptr handle) const;
  int size() const;

  void setAppId(quintptr handle, const QString &appId);
  void setTitle(quintptr handle, const QString &title);
  /** Replace the state bitfield (indices, not masks). */
  void setStateIndices(quintptr handle, const QList<quint32> &indices);
  /** Apply the atomic `done` batch. */
  void commit(quintptr handle);
  void remove(quintptr handle);

  /** Committed windows, sorted by key for stable snapshots. */
  QList<WindowInfo> snapshot() const;

private:
  struct Record {
    quintptr handle = 0;
    QString appId;
    QString title;
    quint32 state = 0; // bitfield by State index
    bool announced = false;
  };

  QHash<quintptr, Record> m_records;
  quintptr m_next = 1;
};

} // namespace TrophyBackend
