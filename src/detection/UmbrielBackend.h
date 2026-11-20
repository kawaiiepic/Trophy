/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "WindowInfo.h"
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>

namespace TrophyBackend {

// Reads Umbriel's read-only JSON IPC via its CLI; no compositor-side helper
// is installed. Opaque foreign toplevel IDs are represented by stable keys.
class UmbrielBackend : public QObject {
  Q_OBJECT
public:
  explicit UmbrielBackend(QObject *parent = nullptr);
  ~UmbrielBackend() override = default;

  bool start();
  void stop() { m_active.store(false); }
  bool isActive() const { return m_active.load(); }
  void refresh();
  QList<WindowInfo> windows() const;

  static QList<WindowInfo> parseWindows(const QByteArray &windowsJson,
                                         const QByteArray &focusedJson = {},
                                         const QByteArray &outputsJson = {});
  static QString signatureFor(const QList<WindowInfo> &windows);

signals:
  void windowsUpdated();

private:
  static QByteArray runUmbriel(const QStringList &args);
  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  QString m_lastSignature;
  std::atomic_bool m_active{false};
};
} // namespace TrophyBackend
