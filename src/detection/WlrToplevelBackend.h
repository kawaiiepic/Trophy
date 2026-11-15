/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "WindowInfo.h"

#include <QList>
#include <QObject>
#include <QString>

#include <memory>

namespace TrophyBackend {

/**
 * @brief Window tracking for wlroots-family compositors.
 *
 * Binds zwlr_foreign_toplevel_manager_v1, which Sway, Hyprland, labwc,
 * river, Wayfire and niri all expose: app_id, title, and
 * activated/fullscreen/minimized state, event-driven and with nothing
 * installed. The protocol has no PID or geometry, so this backend runs as
 * the primary list and takes PID enrichment from XWayland or a compositor
 * CLI where one exists.
 */
class WlrToplevelBackend : public QObject {
  Q_OBJECT

public:
  class Private;

  explicit WlrToplevelBackend(QObject *parent = nullptr);
  ~WlrToplevelBackend() override;

  static bool isAvailable();

  /** @return false when the compositor does not expose the protocol. */
  bool start(QString *error = nullptr);
  void stop();
  bool isActive() const;
  bool hasSnapshot() const;
  QList<WindowInfo> windows() const;

signals:
  void windowsUpdated();

private:
  std::unique_ptr<Private> d;
};

} // namespace TrophyBackend
