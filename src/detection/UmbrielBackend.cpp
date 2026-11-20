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
#include <QMutexLocker>
#include <QProcess>
#include <QSize>
#include <QStandardPaths>
#include <algorithm>

namespace TrophyBackend {

UmbrielBackend::UmbrielBackend(QObject *parent) : QObject(parent) {}

bool UmbrielBackend::start() {
  if (QStandardPaths::findExecutable("umbriel").isEmpty()) {
    qWarning() << "Umbriel backend: umbriel CLI not found on PATH";
    return false;
  }
  qInfo() << "Umbriel window tracking armed (read-only JSON IPC polling)";
  return true;
}

QByteArray UmbrielBackend::runUmbriel(const QStringList &args) {
  const QString executable = QStandardPaths::findExecutable("umbriel");
  if (executable.isEmpty())
    return {};
  QProcess process;
  process.start(executable, args);
  if (!process.waitForStarted(1500) || !process.waitForFinished(2000)) {
    process.kill();
    process.waitForFinished(500);
    return {};
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {};
  return process.readAllStandardOutput();
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
  const QByteArray rawWindows = runUmbriel({"windows", "--json"});
  // A disconnected IPC socket must not keep reporting helper "Working".
  const QJsonDocument document = QJsonDocument::fromJson(rawWindows);
  if (!document.isArray()) {
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
  const QList<WindowInfo> snapshot =
      parseWindows(rawWindows, runUmbriel({"focused-window", "--json"}),
                   runUmbriel({"outputs", "--json"}));
  const QString signature = signatureFor(snapshot);
  {
    QMutexLocker lock(&m_mutex);
    m_active.store(true); // Valid empty array still means healthy helper.
    if (signature == m_lastSignature)
      return;
    m_lastSignature = signature;
    m_windows = snapshot;
  }
  emit windowsUpdated();
}

} // namespace TrophyBackend
