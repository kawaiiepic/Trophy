/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GlobalShortcutManager.h"

#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QCoreApplication>
#include <QDebug>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QUuid>

namespace TrophyBackend {

namespace {
const char *kService = "org.freedesktop.portal.Desktop";
const char *kPath = "/org/freedesktop/portal/desktop";
const char *kInterface = "org.freedesktop.portal.GlobalShortcuts";
const char *kRequestInterface = "org.freedesktop.portal.Request";

QString newToken() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
}

bool validPortalPath(const QString &path) {
  static const QRegularExpression pattern(
      QStringLiteral("\\A(/[A-Za-z0-9_]+)+\\z"));
  return pattern.match(path).hasMatch();
}

QString requestPathFromReply(const QDBusMessage &reply) {
  if (reply.type() != QDBusMessage::ReplyMessage ||
      reply.arguments().size() != 1 ||
      reply.arguments().first().metaType() != QMetaType::fromType<QDBusObjectPath>())
    return {};
  const QString path = reply.arguments().first().value<QDBusObjectPath>().path();
  return validPortalPath(path) ? path : QString();
}

QDBusMessage portalCall(const QString &method, const QVariantList &arguments) {
  auto message = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
  message.setArguments(arguments);
  return message;
}
} // namespace

GlobalShortcutManager::GlobalShortcutManager(QObject *parent)
    : QObject(parent) {
  qDBusRegisterMetaType<PortalShortcut>();
  qDBusRegisterMetaType<QList<PortalShortcut>>();
  m_requestTimeout.setSingleShot(true);
  m_requestTimeout.setInterval(10000);
  connect(&m_requestTimeout, &QTimer::timeout, this,
           &GlobalShortcutManager::onRequestTimeout);
  m_availabilityTimeout.setSingleShot(true);
  m_availabilityTimeout.setInterval(5000);
  connect(&m_availabilityTimeout, &QTimer::timeout, this,
          [this] { setAvailable(false); });
  auto *service = new QDBusServiceWatcher(kService, QDBusConnection::sessionBus(),
      QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(service, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString &, const QString &oldOwner, const QString &) {
    if (!oldOwner.isEmpty()) {
      clearRequest();
      retireUnboundSession();
    }
    queryAvailability();
  });
  queryAvailability();
  ensureResponseSubscription();
}

bool GlobalShortcutManager::isAvailable() const {
  return m_available;
}

void GlobalShortcutManager::queryAvailability() {
  const quint64 generation = ++m_availabilityGeneration;
  m_availabilityTimeout.start();
  auto message = QDBusMessage::createMethodCall(
      kService, kPath, "org.freedesktop.DBus.Properties", "Get");
  message.setArguments({QString::fromLatin1(kInterface), QStringLiteral("version")});
  auto *watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(message, m_availabilityTimeout.interval()), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, generation](QDBusPendingCallWatcher *finished) {
    const auto reply = finished->reply();
    finished->deleteLater();
    if (generation != m_availabilityGeneration)
      return;
    bool available = false;
    if (reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 1 &&
        reply.arguments().first().metaType() == QMetaType::fromType<QDBusVariant>()) {
      const QVariant version = reply.arguments().first().value<QDBusVariant>().variant();
      available = version.metaType() == QMetaType::fromType<uint>() && version.toUInt() > 0;
      m_portalVersion = available ? version.toUInt() : 0;
    } else {
      m_portalVersion = 0;
    }
    setAvailable(available);
  });
}

void GlobalShortcutManager::setAvailable(bool available) {
  ++m_availabilityGeneration;
  m_availabilityTimeout.stop();
  if (m_available == available)
    return;
  m_available = available;
  emit availabilityChanged();
}

void GlobalShortcutManager::disconnectSessionSignals() {
  QDBusConnection::sessionBus().disconnect(
      QString::fromLatin1(kService), QString::fromLatin1(kPath),
      QString::fromLatin1(kInterface), "Activated", this,
      SLOT(handleActivated(QDBusObjectPath, QString)));
  QDBusConnection::sessionBus().disconnect(
      QString::fromLatin1(kService), QString::fromLatin1(kPath),
      QString::fromLatin1(kInterface), "ShortcutsChanged", this,
      SLOT(onShortcutsChanged(QDBusObjectPath, QList<PortalShortcut>)));
}

void GlobalShortcutManager::onRequestTimeout() {
  if (!m_requestInFlight)
    return;
  qWarning() << "GlobalShortcuts portal request timed out; a later"
                "ensureSetup can start a fresh flow";
  clearRequest();
  // A lost BindShortcuts response leaves an unbound session behind.
  // Retire it too, otherwise requestShortcuts rejects every fresh retry.
  retireUnboundSession();
}

quint64 GlobalShortcutManager::beginRequest(bool binding) {
  const quint64 generation = ++m_requestGeneration;
  m_requestInFlight = true;
  m_bindingRequest = binding;
  m_requestTimeout.start();
  return generation;
}

void GlobalShortcutManager::ensureResponseSubscription() {
  if (m_responseSubscribed)
    return;
  // Same race as the ScreenCast flow: the portal can emit Response
  // within milliseconds of the method reply, before a connect issued
  // after the reply registers its daemon-side match rule. Subscribe
  // once with no path filter; both Response slots key off
  // m_pendingRequestPath and the request generation.
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), QString(),
      QString::fromLatin1(kRequestInterface), "Response", this,
      SLOT(onSessionResponse(unsigned int, QVariantMap, QDBusMessage)));
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), QString(),
      QString::fromLatin1(kRequestInterface), "Response", this,
      SLOT(onBindResponse(unsigned int, QVariantMap, QDBusMessage)));
  m_responseSubscribed = true;
}

void GlobalShortcutManager::setLastError(const QString &message) {
  if (m_lastError == message)
    return;
  m_lastError = message;
  emit lastErrorChanged();
}

void GlobalShortcutManager::clearRequest() {
  m_requestTimeout.stop();
  // No per-request disconnect: the Response subscription is permanent
  // (see ensureResponseSubscription) and both slots ignore anything
  // that is not the current pending path/generation.
  m_pendingRequestPath.clear();
  m_requestInFlight = false;
  m_bindingRequest = false;
  ++m_requestGeneration;
}

void GlobalShortcutManager::watchRequest(const QDBusPendingCall &call,
                                        quint64 generation, bool binding) {
  auto *watcher = new QDBusPendingCallWatcher(call, this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, generation, binding](QDBusPendingCallWatcher *finished) {
    const QDBusMessage reply = finished->reply();
    finished->deleteLater();
    if (!m_requestInFlight || generation != m_requestGeneration)
      return;
    const QString requestPath = requestPathFromReply(reply);
    if (requestPath.isEmpty()) {
      qWarning() << "GlobalShortcuts" << (binding ? "BindShortcuts" : "CreateSession")
                 << "failed or gave no valid request path:" << reply.errorMessage();
      const QString detail = reply.errorMessage().trimmed();
      setLastError(detail.isEmpty()
                       ? (binding ? tr("The system rejected the shortcut binding.")
                                  : tr("The system could not open a shortcut session."))
                       : detail);
      clearRequest();
      retireUnboundSession();
      return;
    }
    // No per-reply connect: the permanent subscription from
    // ensureResponseSubscription() already covers this path. Just
    // publish the dispatch key the Response slot matches on.
    m_pendingRequestPath = requestPath;
    setAvailable(true); // A valid method reply proves interface support too.
    // Do not restart the timer: method wait and Response share one budget.
  });
}

void GlobalShortcutManager::retireUnboundSession() {
  disconnectSessionSignals();
  m_sessionHandle.clear();
  // A portal may push ShortcutsChanged before its Bind response. Such a
  // map cannot remain usable after the owning session has been retired.
  applyBoundShortcuts({});
}

QList<PortalShortcut> GlobalShortcutManager::shortcutDefinitions() {
  PortalShortcut toggle;
  toggle.id = QString::fromLatin1(kToggleRecordingId);
  toggle.options["description"] = QCoreApplication::translate(
      "GlobalShortcutManager", "Start/stop recording the detected game");
  toggle.options["preferred_trigger"] = QStringLiteral("F9");
  PortalShortcut clip;
  clip.id = QString::fromLatin1(kSaveClipId);
  clip.options["description"] = QCoreApplication::translate(
      "GlobalShortcutManager", "Save a replay clip, or bookmark the current recording");
  clip.options["preferred_trigger"] = QStringLiteral("F10");
  return {toggle, clip};
}

QVariantMap
GlobalShortcutManager::boundMap(const QList<PortalShortcut> &shortcuts) {
  QVariantMap bound;
  for (const PortalShortcut &s : shortcuts) {
    if (s.id == QLatin1String(kToggleRecordingId) || s.id == QLatin1String(kSaveClipId))
      bound.insert(s.id, s.options.value("trigger_description").toString());
  }
  return bound;
}

void GlobalShortcutManager::requestShortcuts() {
  if (m_bound || m_requestInFlight || !m_pendingRequestPath.isEmpty() ||
      !m_sessionHandle.isEmpty())
    return;
  const quint64 generation = beginRequest(false);
  QVariantMap options;
  options["handle_token"] = newToken();
  options["session_handle_token"] = newToken();
  watchRequest(QDBusConnection::sessionBus().asyncCall(
      portalCall("CreateSession", {options}), m_requestTimeout.interval()), generation, false);
}

void GlobalShortcutManager::onSessionResponse(unsigned int response,
                                              const QVariantMap &results,
                                              const QDBusMessage &message) {
  if (!m_requestInFlight || m_bindingRequest || m_pendingRequestPath.isEmpty() ||
      (!message.path().isEmpty() && message.path() != m_pendingRequestPath))
    return;
  clearRequest();
  if (response != 0) {
    qWarning() << "GlobalShortcuts session creation failed or was cancelled";
    return;
  }
  m_sessionHandle = results.value("session_handle").toString();
  if (!validPortalPath(m_sessionHandle)) {
    m_sessionHandle.clear();
    qWarning() << "GlobalShortcuts session gave no handle";
    return;
  }
  setAvailable(true);

  // Activations + remaps for our session.
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), QString::fromLatin1(kPath),
      QString::fromLatin1(kInterface), "Activated", this,
      SLOT(handleActivated(QDBusObjectPath, QString)));
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), QString::fromLatin1(kPath),
      QString::fromLatin1(kInterface), "ShortcutsChanged", this,
      SLOT(onShortcutsChanged(QDBusObjectPath, QList<PortalShortcut>)));

  bindShortcuts();
}

void GlobalShortcutManager::bindShortcuts() {
  if (m_sessionHandle.isEmpty() || m_requestInFlight ||
      !m_pendingRequestPath.isEmpty())
    return;
  const quint64 generation = beginRequest(true);
  QVariantMap options;
  options["handle_token"] = newToken();
  watchRequest(QDBusConnection::sessionBus().asyncCall(portalCall(
      "BindShortcuts", {QVariant::fromValue(QDBusObjectPath(m_sessionHandle)),
      QVariant::fromValue(shortcutDefinitions()), QString(), options}),
      m_requestTimeout.interval()), generation, true);
}

void GlobalShortcutManager::onBindResponse(unsigned int response,
                                           const QVariantMap &results,
                                           const QDBusMessage &message) {
  if (!m_requestInFlight || !m_bindingRequest || m_pendingRequestPath.isEmpty() ||
      (!message.path().isEmpty() && message.path() != m_pendingRequestPath))
    return;
  clearRequest();
  if (response != 0) {
    qWarning() << "GlobalShortcuts bind failed or was cancelled";
    setLastError(tr("The system cancelled the shortcut approval."));
    // A cancelled bind leaves no usable session: drop the handle and
    // its signal connections so a retry goes through the full
    // CreateSession flow instead of landing on reconfigure forever.
    retireUnboundSession();
    return;
  }
  QList<PortalShortcut> bound;
  const QVariant raw = results.value("shortcuts");
  if (raw.canConvert<QDBusArgument>()) {
    bound = qdbus_cast<QList<PortalShortcut>>(raw.value<QDBusArgument>());
  }
  if (bound.isEmpty()) {
    qWarning() << "GlobalShortcuts bind returned no shortcuts";
    // A successful Response with unusable data is not a usable bind.
    // Keeping its handle strands ensureSetup on ConfigureShortcuts rather
    // than permitting a fresh CreateSession/BindShortcuts retry.
    retireUnboundSession();
    return;
  }
  applyBoundShortcuts(bound);
  setLastError(QString());
  qDebug() << "Global shortcuts bound:" << m_boundShortcuts.keys();
}

void GlobalShortcutManager::applyBoundShortcuts(
    const QList<PortalShortcut> &shortcuts) {
  const QVariantMap next = boundMap(shortcuts);
  const bool bound = !next.isEmpty();
  // Compare the whole map, not just the bool: remapping keys while still
  // bound must still notify, or the UI keeps showing the old triggers
  // until the app restarts.
  if (next == m_boundShortcuts && bound == m_bound)
    return;
  m_boundShortcuts = next;
  m_bound = bound;
  emit boundChanged();
}

void GlobalShortcutManager::onShortcutsChanged(
    const QDBusObjectPath &sessionHandle,
    const QList<PortalShortcut> &shortcuts) {
  if (sessionHandle.path() != m_sessionHandle)
    return;
  qDebug() << "Portal reported shortcut change:" << boundMap(shortcuts);
  applyBoundShortcuts(shortcuts);
}

void GlobalShortcutManager::ensureSetup() {
  if (m_requestInFlight || !m_pendingRequestPath.isEmpty())
    return;
  // requestShortcuts no-ops when a session already exists or a request
  // is in flight; reconfigureShortcuts falls back to it without a
  // session. Exactly one path acts.
  if (m_sessionHandle.isEmpty())
    requestShortcuts();
  else
    reconfigureShortcuts();
}

void GlobalShortcutManager::reconfigureShortcuts() {
  if (m_requestInFlight || !m_pendingRequestPath.isEmpty())
    return;
  if (m_sessionHandle.isEmpty()) {
    requestShortcuts();
    return;
  }
  if (m_portalVersion < 2) {
    setLastError(tr("Your desktop's shortcuts portal does not support an in-app editor. Edit the desktop's shortcut configuration instead."));
    return;
  }
  // Empirical (KDE 6.x GlobalShortcuts backend): a repeat BindShortcuts
  // on a live session returns the existing bindings WITHOUT showing any
  // dialog, so it cannot be used to re-pick keys. ConfigureShortcuts is
  // the spec'd way to ask, and it does show UI — on KDE that means the
  // System Settings keybinds page rather than a compact prompt. Better a
  // heavyweight dialog than a button that does nothing.
  //
  // Shortcuts are configured exclusively through the portal: the desktop's
  // GlobalShortcuts backend owns the UI (KDE and GNOME both ship one).
  // Watch the reply only to surface a rejection; never launch compositor
  // settings, which would not edit the portal-bound triggers anyway.
  QVariantMap options;
  auto call = portalCall("ConfigureShortcuts",
      {QVariant::fromValue(QDBusObjectPath(m_sessionHandle)), QString(), options});
  auto pending = QDBusConnection::sessionBus().asyncCall(call, 10000);
  auto *watcher = new QDBusPendingCallWatcher(pending, this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this](QDBusPendingCallWatcher *finished) {
    const QDBusMessage reply = finished->reply();
    finished->deleteLater();
    if (reply.type() == QDBusMessage::ReplyMessage) {
      qDebug() << "ConfigureShortcuts accepted by the portal";
      return;
    }
    qWarning() << "ConfigureShortcuts failed:" << reply.errorMessage();
    const QString detail = reply.errorMessage().trimmed();
    setLastError(detail.isEmpty()
                     ? tr("The system could not open the shortcut editor.")
                     : detail);
  });
}

QString UmbrielAppId() {
  QString id = QGuiApplication::desktopFileName();
  if (id.endsWith(QStringLiteral(".desktop")))
    id.chop(8);
  if (id.isEmpty())
    id = QStringLiteral("io.github.stevenjwheeler.Trophy");
  return id;
}

bool GlobalShortcutManager::validUmbrielChord(const QString &chord) {
  // Allow empty to explicitly disable a shortcut. Keep the accepted input
  // intentionally narrow so it can be represented safely as both JSON and
  // a TOML string without escaping arbitrary user-provided characters.
  static const QRegularExpression pattern(
      QStringLiteral(R"(\A[A-Za-z0-9_-]+(?:\+[A-Za-z0-9_-]+){0,5}\z)"));
  return chord.isEmpty() || (chord.size() <= 80 && pattern.match(chord).hasMatch());
}

QString GlobalShortcutManager::umbrielShortcutTable(
    const QString &recordKey, const QString &clipKey) {
  if (!validUmbrielChord(recordKey) || !validUmbrielChord(clipKey) ||
      (!recordKey.isEmpty() && recordKey == clipKey))
    return {};
  return QStringLiteral("[global_shortcuts.\"%1\"]\n"
                        "\"toggle-recording\" = \"%2\"\n"
                        "\"save-clip\" = \"%3\"\n")
      .arg(UmbrielAppId(), recordKey, clipKey);
}

bool GlobalShortcutManager::copyUmbrielShortcutConfiguration(
    const QString &recordKey, const QString &clipKey) {
  const QString snippet = umbrielShortcutTable(recordKey, clipKey);
  if (snippet.isEmpty()) {
    setLastError(tr("Enter valid, different keyboard chords (or leave a chord empty to disable it)."));
    return false;
  }
  if (!QGuiApplication::clipboard()) {
    setLastError(tr("The clipboard is unavailable."));
    return false;
  }
  QGuiApplication::clipboard()->setText(snippet);
  setLastError({});
  return true;
}

bool GlobalShortcutManager::applyUmbrielShortcuts(
    const QString &recordKey, const QString &clipKey) {
  const QString snippet = umbrielShortcutTable(recordKey, clipKey);
  if (snippet.isEmpty()) {
    setLastError(tr("Enter valid, different keyboard chords (or leave a chord empty to disable it)."));
    return false;
  }
  // Direct IPC avoids launching the compositor binary inside the AppImage.
  const QString socketOverride = qEnvironmentVariable("UMBRIEL_SOCKET");
  const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  const QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
  const QString socketPath = !socketOverride.isEmpty()
      ? socketOverride
      : (!runtime.isEmpty() && !display.isEmpty()
          ? runtime + QStringLiteral("/umbriel-") + display + QStringLiteral(".sock")
          : QString());
  if (socketPath.isEmpty()) {
    setLastError(tr("Cannot find Umbriel's IPC socket. Check WAYLAND_DISPLAY and XDG_RUNTIME_DIR."));
    return false;
  }

  const QString table = QStringLiteral("{\"toggle-recording\" = \"%1\", "
                                       "\"save-clip\" = \"%2\"}")
                             .arg(recordKey, clipKey);
  const QString key = QStringLiteral("global_shortcuts.\"%1\"").arg(UmbrielAppId());
  QJsonObject request;
  request.insert(QStringLiteral("cmd"), QStringLiteral("config-set"));
  request.insert(QStringLiteral("args"), QJsonArray{key, table});
  const QString token = qEnvironmentVariable("UMBRIEL_LAUNCH_TOKEN");
  if (!token.isEmpty())
    request.insert(QStringLiteral("launch_token"), token);

  QLocalSocket socket;
  socket.connectToServer(socketPath, QIODevice::ReadWrite);
  if (!socket.waitForConnected(1500)) {
    setLastError(tr("Cannot connect to Umbriel: %1").arg(socket.errorString()));
    return false;
  }
  const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
  if (socket.write(bytes) != bytes.size() ||
      (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(1500))) {
    setLastError(tr("Could not send the shortcut change to Umbriel: %1").arg(socket.errorString()));
    return false;
  }
  QByteArray response;
  QElapsedTimer timer;
  timer.start();
  while (!response.contains('\n') && timer.elapsed() < 2500) {
    response += socket.readAll();
    if (response.size() > 65536)
      break;
    if (response.contains('\n'))
      break;
    if (socket.state() == QLocalSocket::UnconnectedState)
      break;
    socket.waitForReadyRead(qMax(1, 2500 - int(timer.elapsed())));
  }
  response += socket.readAll();
  const int end = response.indexOf('\n');
  if (end < 0 || end > 65536) {
    setLastError(tr("Umbriel did not acknowledge the shortcut change."));
    return false;
  }
  QJsonParseError error;
  const QJsonDocument reply = QJsonDocument::fromJson(response.left(end), &error);
  if (error.error != QJsonParseError::NoError || !reply.isObject()) {
    setLastError(tr("Umbriel returned an invalid shortcut response."));
    return false;
  }
  const QJsonObject result = reply.object();
  if (!result.contains(QStringLiteral("ok"))) {
    setLastError(result.value(QStringLiteral("err")).toString(
        tr("Umbriel refused the requested shortcut bindings.")));
    return false;
  }
  setLastError({});
  qInfo() << "Umbriel shortcut overrides applied for this session";
  return true;
}

void GlobalShortcutManager::handleActivated(
    const QDBusObjectPath &sessionHandle, const QString &id) {
  if (sessionHandle.path() != m_sessionHandle || m_sessionHandle.isEmpty())
    return;
  simulateActivation(id);
}

void GlobalShortcutManager::simulateActivation(const QString &id) {
  if (!m_enabled)
    return; // switched off in Settings: swallow the keypress
  if (id == QLatin1String(kToggleRecordingId)) {
    emit toggleRecordingRequested();
  } else if (id == QLatin1String(kSaveClipId)) {
    emit saveClipRequested();
  }
}

void GlobalShortcutManager::setEnabled(bool enabled) {
  if (m_enabled != enabled) {
    m_enabled = enabled;
    emit enabledChanged();
  }
}

} // namespace TrophyBackend
