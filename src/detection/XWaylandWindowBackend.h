/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "WindowInfo.h"

#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <atomic>

namespace TrophyBackend {

/** Property bytes for one X11 client window, before interpretation. */
struct X11WindowRecord {
  quint32 id = 0;
  quint32 pid = 0;
  QString wmClass;      // second WM_CLASS field (the class)
  QString title;        // _NET_WM_NAME, or WM_NAME
  QList<quint32> state; // _NET_WM_STATE atoms
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool viewable = true;
};

struct X11Atoms {
  quint32 fullscreen = 0;
  quint32 hidden = 0;
  quint32 skipTaskbar = 0;
};

/**
 * @brief Windows of XWayland clients, with their real process ids.
 *
 * Wayland exposes no window->process mapping, but X11 does: every toolkit
 * sets `_NET_WM_PID` on its window, and the compositor's XWM publishes
 * `_NET_CLIENT_LIST`/`_NET_ACTIVE_WINDOW` on the root window. That works on
 * KWin, Mutter and wlroots alike, and it is where Proton/Wine games run, so
 * it supplies the PID that listing-only Wayland protocols cannot.
 *
 * Read-only: one short-lived X connection per poll, no installation and no
 * window contents. `parseWindows()` is pure and unit-tested against a fake
 * window table; the live path adds only the connection plumbing.
 */
class XWaylandWindowBackend : public QObject {
  Q_OBJECT

public:
  explicit XWaylandWindowBackend(QObject *parent = nullptr);
  ~XWaylandWindowBackend() override = default;

  /** A usable X display is configured in the environment. */
  static bool isAvailable();

  /** Connect, resolve the root atoms and prove the client list exists. */
  bool start();
  void stop();
  bool isActive() const { return m_active.load(); }

  /** One poll; called from the monitor's watchdog worker. */
  void refresh();

  QList<WindowInfo> windows() const;
  /** True once a poll completed; an empty desktop still counts. */
  bool hasSnapshot() const { return m_hasSnapshot.load(); }

  /**
   * @brief Turn an X11 client window table into monitor snapshots.
   *
   * The class is lowercased because compositors derive `app_id` from the
   * same WM_CLASS field; tooltips/docks (`skip_taskbar`) and unmapped
   * windows are dropped, and `_NET_ACTIVE_WINDOW` marks focus.
   */
  static QList<WindowInfo> parseWindows(const QList<X11WindowRecord> &records,
                                        quint32 activeId, const X11Atoms &atoms);

  /** Content signature, so unchanged polls do not emit. */
  static QString signatureFor(const QList<WindowInfo> &windows);

signals:
  void windowsUpdated();

private:
  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  QString m_lastSignature;
  std::atomic_bool m_active{false};
  std::atomic_bool m_hasSnapshot{false};
};

} // namespace TrophyBackend
