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
 * @brief Window tracking on KDE through org_kde_plasma_window_management.
 *
 * KWin exposes its own window-management protocol to ordinary clients, with
 * app_id, title, pid, state (active/minimized/fullscreen) and geometry. That
 * makes the KWin script optional on Plasma: no package install, no
 * kpackagetool6, no D-Bus round-trips, and no helper status to explain. The
 * script remains the fallback when the protocol is missing or too old.
 *
 * The protocol is a KDE implementation detail (its own XML warns regular
 * clients off it), so callers must treat a failed start() as "use the
 * script" and remain functional if a future Plasma drops the global.
 */
class KWinProtocolBackend : public QObject {
  Q_OBJECT

public:
  class Private;

  explicit KWinProtocolBackend(QObject *parent = nullptr);
  ~KWinProtocolBackend() override;

  /** Compiled support and a usable Wayland session in the environment. */
  static bool isAvailable();

  /**
   * @brief Connect, bind the global and request the current window list.
   *
   * @return false when the compositor does not expose a usable global (not
   *         KDE, too old, or no Wayland session); callers fall back.
   */
  bool start(QString *error = nullptr);
  void stop();
  bool isActive() const;

  /** True once a window list arrived; an empty desktop still counts. */
  bool hasSnapshot() const;

  QList<WindowInfo> windows() const;
  /** Bound protocol version (0 when inactive). */
  int protocolVersion() const;

signals:
  void windowsUpdated();

private:
  std::unique_ptr<Private> d;
};

} // namespace TrophyBackend
