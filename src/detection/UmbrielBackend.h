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
#include <atomic>

namespace TrophyBackend {

// Talks to Umbriel's read-only Unix socket without executing its CLI.
// Opaque foreign toplevel IDs are represented by stable keys.
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

  // The raw socket replies wrap successful command data in {"ok": ...}.
  static QByteArray unwrapReply(const QByteArray &reply, QString *error = nullptr);

signals:
  void windowsUpdated();

private:
  static QString socketPath();
  static QByteArray query(const QString &command, QString *error = nullptr);
  QString m_lastError;
  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  QString m_lastSignature;
  std::atomic_bool m_active{false};
};
} // namespace TrophyBackend
