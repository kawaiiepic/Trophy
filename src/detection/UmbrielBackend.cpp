/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "UmbrielBackend.h"

#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QElapsedTimer>
#include <QJsonValue>
#include <QLocalSocket>
#include <QMutexLocker>
#include <QSize>
#include <algorithm>

namespace TrophyBackend {

UmbrielBackend::UmbrielBackend(QObject *parent) : QObject(parent) {}

QString UmbrielBackend::socketPath() {
  // Match Umbriel's own CLI path selection. UMBRIEL_SOCKET permits
  // nested sessions to use an explicit socket.
  const QString override = qEnvironmentVariable("UMBRIEL_SOCKET");
  if (!override.isEmpty())
    return override;
  const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  const QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
  if (runtime.isEmpty() || display.isEmpty())
    return {};
  return runtime + QStringLiteral("/umbriel-") + display +
         QStringLiteral(".sock");
}

bool UmbrielBackend::start() {
  const QString path = socketPath();
  if (path.isEmpty()) {
    qWarning() << "Umbriel window tracking: cannot locate IPC socket."
                  " Check XDG_RUNTIME_DIR, WAYLAND_DISPLAY and UMBRIEL_SOCKET.";
    return false;
  }
  qInfo() << "Umbriel window tracking armed (native read-only socket IPC)"
          << path;
  return true;
}

// Umbriel IPC is one newline-delimited JSON object per request and reply.
// Unlike the CLI, its raw socket reply is wrapped in {"ok": ...}.
QByteArray UmbrielBackend::unwrapReply(const QByteArray &reply,
                                      QString *error) {
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(reply, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    if (error)
      *error = QStringLiteral("invalid JSON reply");
    return {};
  }
  const QJsonObject response = doc.object();
  if (response.contains("err")) {
    if (error)
      *error = response.value("err").toString(QStringLiteral("IPC error"));
    return {};
  }
  if (!response.contains("ok")) {
    if (error)
      *error = QStringLiteral("reply missing 'ok'");
    return {};
  }
  const QJsonValue ok = response.value("ok");
  if (ok.isArray())
    return QJsonDocument(ok.toArray()).toJson(QJsonDocument::Compact);
  if (ok.isObject())
    return QJsonDocument(ok.toObject()).toJson(QJsonDocument::Compact);
  if (ok.isNull())
    return QByteArrayLiteral("null");
  if (error)
    *error = QStringLiteral("unexpected IPC response type");
  return {};
}

QByteArray UmbrielBackend::query(const QString &command, QString *error) {
  const QString path = socketPath();
  if (path.isEmpty()) {
    if (error)
      *error = QStringLiteral("UMBRIEL_SOCKET or XDG_RUNTIME_DIR/WAYLAND_DISPLAY missing");
    return {};
  }
  QLocalSocket socket;
  socket.connectToServer(path, QIODevice::ReadWrite);
  if (!socket.waitForConnected(1200)) {
    if (error)
      *error = QStringLiteral("cannot connect to %1: %2")
                   .arg(path, socket.errorString());
    return {};
  }

  QJsonObject request;
  request.insert(QStringLiteral("cmd"), command);
  const QString token = qEnvironmentVariable("UMBRIEL_LAUNCH_TOKEN");
  if (!token.isEmpty())
    request.insert(QStringLiteral("launch_token"), token);
  const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact)
                          + '\n';
  if (socket.write(line) != line.size()) {
    if (error)
      *error = QStringLiteral("IPC request write failed: %1").arg(socket.errorString());
    return {};
  }
  if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(1200)) {
    if (error)
      *error = QStringLiteral("IPC request send timed out: %1").arg(socket.errorString());
    return {};
  }

  // Umbriel sends one complete JSON line and closes the connection. Read
  // the line without depending on EOF or launching a second executable
  // inside Trophy's bundled AppImage runtime.
  QByteArray buffer;
  QElapsedTimer clock;
  clock.start();
  constexpr int timeoutMs = 2000;
  constexpr qsizetype maxReplyBytes = 4 * 1024 * 1024;
  while (!buffer.contains('\n') && clock.elapsed() < timeoutMs) {
    buffer += socket.readAll();
    if (buffer.size() > maxReplyBytes) {
      if (error)
        *error = QStringLiteral("IPC reply exceeded size limit");
      return {};
    }
    if (buffer.contains('\n'))
      break;
    if (socket.state() == QLocalSocket::UnconnectedState)
      break;
    socket.waitForReadyRead(qMax(1, timeoutMs - int(clock.elapsed())));
  }
  buffer += socket.readAll();
  if (buffer.size() > maxReplyBytes) {
    if (error)
      *error = QStringLiteral("IPC reply exceeded size limit");
    return {};
  }
  const int newline = buffer.indexOf('\n');
  if (newline < 0) {
    if (error)
      *error = QStringLiteral("IPC reply timed out or closed early: %1")
                   .arg(socket.errorString());
    return {};
  }
  return unwrapReply(buffer.left(newline), error);
}

QList<WindowInfo> UmbrielBackend::parseWindows(
    const QByteArray &windowsJson, const QByteArray &focusedJson,
    const QByteArray &outputsJson) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(windowsJson, &error);
  if (error.error != QJsonParseError::NoError || !document.isArray())
    return {};

  QString focusedId;
  if (!focusedJson.isEmpty()) {
    const QJsonDocument focus = QJsonDocument::fromJson(focusedJson);
    if (focus.isObject())
      focusedId = focus.object().value("id").toVariant().toString();
  }

  QHash<QString, QSize> outputSizes;
  if (!outputsJson.isEmpty()) {
    const QJsonDocument outputs = QJsonDocument::fromJson(outputsJson);
    if (outputs.isArray()) {
      for (const QJsonValue &value : outputs.array()) {
        if (!value.isObject())
          continue;
        const QJsonObject output = value.toObject();
        const QString name = output.value("name").toString();
        const QJsonObject size = output.value("logical_size").toObject();
        const int width = size.value("width").toInt();
        const int height = size.value("height").toInt();
        if (!name.isEmpty() && width > 0 && height > 0)
          outputSizes.insert(name, QSize(width, height));
      }
    }
  }

  QList<WindowInfo> windows;
  for (const QJsonValue &value : document.array()) {
    if (!value.isObject())
      continue;
    const QJsonObject object = value.toObject();
    const QString identifier = object.value("id").toVariant().toString();
    if (identifier.isEmpty())
      continue;
    WindowInfo window;
    // Umbriel IDs are opaque; the monitor hashes key into a picker ID.
    window.key = QStringLiteral("umbriel:") + identifier;
    window.appId = object.value("app_id").toString();
    window.title = object.value("title").toString();
    window.pid = object.value("pid").toVariant().toLongLong();
    if (window.pid < 1)
      window.pid = 0; // Unknown XWayland PID is -1.
    window.fullscreen = object.value("fullscreen").toBool();
    window.minimized = object.value("minimized").toBool();
    window.focused = focusedId.isEmpty()
        ? object.value("active").toBool()
        : identifier == focusedId;
    window.x = object.value("x").toInt();
    window.y = object.value("y").toInt();
    window.width = object.value("w").toInt();
    window.height = object.value("h").toInt();
    const QSize size = outputSizes.value(object.value("output").toString());
    // An unknown output has QSize(-1, -1). Preserve WindowInfo's
    // zero-for-unknown contract so missing geometry stays nonnegative.
    window.outputWidth = size.isValid() ? size.width() : 0;
    window.outputHeight = size.isValid() ? size.height() : 0;
    windows.append(window);
  }
  std::sort(windows.begin(), windows.end(),
            [](const WindowInfo &a, const WindowInfo &b) {
              return a.key < b.key;
            });
  return windows;
}

QList<WindowInfo> UmbrielBackend::windows() const {
  QMutexLocker lock(&m_mutex);
  return m_windows;
}

QString UmbrielBackend::signatureFor(const QList<WindowInfo> &windows) {
  QString signature;
  for (const WindowInfo &w : windows) {
    signature += w.key + QLatin1Char('|') + w.appId + QLatin1Char('|') +
                 w.title + QLatin1Char('|') + QString::number(w.pid) +
                 QLatin1Char('|') + QString::number(w.focused) +
                 QLatin1Char('|') + QString::number(w.fullscreen) +
                 QLatin1Char('|') + QString::number(w.minimized) +
                 QLatin1Char('|') + QString::number(w.x) +
                 QLatin1Char('|') + QString::number(w.y) +
                 QLatin1Char('|') + QString::number(w.width) +
                 QLatin1Char('|') + QString::number(w.height) +
                 QLatin1Char('|') + QString::number(w.outputWidth) +
                 QLatin1Char('|') + QString::number(w.outputHeight) +
                 QLatin1Char(';');
  }
  return signature;
}

void UmbrielBackend::refresh() {
  QString error;
  const QByteArray rawWindows = query(QStringLiteral("windows"), &error);
  const QJsonDocument doc = QJsonDocument::fromJson(rawWindows);
  if (!doc.isArray()) {
    if (error.isEmpty())
      error = QStringLiteral("window list is not a JSON array");
    if (m_lastError != error) {
      qWarning() << "Umbriel window helper unavailable:" << error;
      m_lastError = error;
    }
    if (m_active.exchange(false)) {
      {
        QMutexLocker lock(&m_mutex);
        m_windows.clear();
        m_lastSignature.clear();
      }
      emit windowsUpdated();
    }
    return;
  }

  const QByteArray focused = query(QStringLiteral("focused-window"));
  const QByteArray outputs = query(QStringLiteral("outputs"));
  const QList<WindowInfo> snapshot = parseWindows(rawWindows, focused, outputs);
  const QString signature = signatureFor(snapshot);
  {
    QMutexLocker lock(&m_mutex);
    const bool wasActive = m_active.exchange(true);
    if (!wasActive)
      qInfo() << "Umbriel IPC connected; game window helper ready";
    m_lastError.clear();
    if (signature == m_lastSignature)
      return;
    m_lastSignature = signature;
    m_windows = snapshot;
  }
  emit windowsUpdated();
}

} // namespace TrophyBackend
