/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KWinProtocolBackend.h"

#include "PlasmaToplevelModel.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QScreen>
#include <QSize>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef HAVE_WAYLAND_TOPLEVEL
#include <wayland-client.h>

#include "plasma-window-management-client-protocol.h"
#endif

namespace TrophyBackend {

#ifdef HAVE_WAYLAND_TOPLEVEL

class KWinProtocolBackend::Private {
public:
  struct WindowData {
    Private *owner = nullptr;
    quintptr handle = 0;
    bool closed = false;
  };

  KWinProtocolBackend *q = nullptr;
  PlasmaToplevelModel model;
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  org_kde_plasma_window_management *management = nullptr;
  QSocketNotifier *readNotifier = nullptr;
  QSocketNotifier *writeNotifier = nullptr;
  QHash<quintptr, org_kde_plasma_window *> windows;
  QList<WindowData *> windowData;
  QHash<QString, quintptr> byUuid;
  quint32 version = 0;
  bool active = false;
  bool hasSnapshot = false;
  bool emitScheduled = false;
  int lastLoggedCount = -1;

  void attach(const QString &uuid);
  void schedule();
  void emitSnapshot();
  void dispatch();
  void flush();
  void fail(const QString &reason);
  void destroyAll();
};

namespace {

using BackendPrivate = KWinProtocolBackend::Private;
using WindowData = KWinProtocolBackend::Private::WindowData;

/** Logical size of the output containing a point, when Qt knows it. */
QSize outputSizeAt(int x, int y) {
  if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
    return {};
  const QPoint point(x, y);
  const auto screens = QGuiApplication::screens();
  for (QScreen *screen : screens) {
    if (screen && screen->geometry().contains(point))
      return screen->geometry().size();
  }
  return {};
}

WindowData *dataFor(void *data) {
  auto *w = static_cast<WindowData *>(data);
  return (w && w->owner) ? w : nullptr;
}

void onTitle(void *data, org_kde_plasma_window *, const char *title) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.setTitle(w->handle, QString::fromUtf8(title ? title : ""));
  w->owner->schedule();
}

void onAppId(void *data, org_kde_plasma_window *, const char *appId) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.setAppId(w->handle, QString::fromUtf8(appId ? appId : ""));
  w->owner->schedule();
}

void onResourceName(void *data, org_kde_plasma_window *,
                    const char *resourceName) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.setResourceName(w->handle,
                                 QString::fromUtf8(resourceName ? resourceName : ""));
  w->owner->schedule();
}

void onState(void *data, org_kde_plasma_window *, uint32_t flags) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.setState(w->handle, flags);
  w->owner->schedule();
}

void onPid(void *data, org_kde_plasma_window *, uint32_t pid) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.setPid(w->handle, pid);
  w->owner->schedule();
}

void onGeometry(void *data, org_kde_plasma_window *, int32_t x, int32_t y,
                uint32_t width, uint32_t height) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.setGeometry(w->handle, x, y, int(width), int(height));
  w->owner->schedule();
}

void onClientGeometry(void *data, org_kde_plasma_window *, int32_t x, int32_t y,
                      uint32_t width, uint32_t height) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.setClientGeometry(w->handle, x, y, int(width), int(height));
  w->owner->schedule();
}

void onInitialState(void *data, org_kde_plasma_window *) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.announce(w->handle);
  w->owner->schedule();
}

void onMapped(void *data, org_kde_plasma_window *) {
  auto *w = dataFor(data);
  if (!w)
    return;
  w->owner->model.announce(w->handle);
  w->owner->schedule();
}

void onUnmapped(void *data, org_kde_plasma_window *) {
  auto *w = dataFor(data);
  if (!w)
    return;
  BackendPrivate *owner = w->owner;
  // The compositor drops the window object itself; keep the proxy alive so a
  // late event can never reach a freed listener, but stop publishing it.
  w->closed = true;
  const QString uuid = owner->model.uuid(w->handle);
  owner->model.remove(w->handle);
  if (!uuid.isEmpty())
    owner->byUuid.remove(uuid);
  owner->schedule();
}

void onNoop(void *, org_kde_plasma_window *) {}
void onNoopStr(void *, org_kde_plasma_window *, const char *) {}
void onNoopInt(void *, org_kde_plasma_window *, int32_t) {}
void onNoopPair(void *, org_kde_plasma_window *, const char *, const char *) {}
void onNoopObject(void *, org_kde_plasma_window *, org_kde_plasma_window *) {}

const org_kde_plasma_window_listener &windowListener() {
  static const org_kde_plasma_window_listener listener = [] {
    org_kde_plasma_window_listener l{};
    l.title_changed = onTitle;
    l.app_id_changed = onAppId;
    l.state_changed = onState;
    l.virtual_desktop_changed = onNoopInt;
    l.themed_icon_name_changed = onNoopStr;
    l.unmapped = onUnmapped;
    l.initial_state = onInitialState;
    l.parent_window = onNoopObject;
    l.geometry = onGeometry;
    l.icon_changed = onNoop;
    l.pid_changed = onPid;
    l.virtual_desktop_entered = onNoopStr;
    l.virtual_desktop_left = onNoopStr;
    l.application_menu = onNoopPair;
    l.activity_entered = onNoopStr;
    l.activity_left = onNoopStr;
    l.resource_name_changed = onResourceName;
    l.client_geometry = onClientGeometry;
    l.mapped = onMapped;
    return l;
  }();
  return listener;
}

void onStackingWindow(void *data, org_kde_plasma_stacking_order *,
                      const char *uuid) {
  auto *p = static_cast<BackendPrivate *>(data);
  if (p && uuid)
    p->attach(QString::fromUtf8(uuid));
}

void onStackingDone(void *data, org_kde_plasma_stacking_order *order) {
  auto *p = static_cast<BackendPrivate *>(data);
  org_kde_plasma_stacking_order_destroy(order);
  if (p)
    p->schedule();
}

const org_kde_plasma_stacking_order_listener &stackingListener() {
  static const org_kde_plasma_stacking_order_listener listener = [] {
    org_kde_plasma_stacking_order_listener l{};
    l.window = onStackingWindow;
    l.done = onStackingDone;
    return l;
  }();
  return listener;
}

void onWindowWithUuid(void *data, org_kde_plasma_window_management *,
                      uint32_t, const char *uuid) {
  auto *p = static_cast<BackendPrivate *>(data);
  if (p && uuid)
    p->attach(QString::fromUtf8(uuid));
}

void onManagerNoop(void *, org_kde_plasma_window_management *, uint32_t) {}
void onManagerNoopStr(void *, org_kde_plasma_window_management *,
                      const char *) {}
void onManagerNoopArray(void *, org_kde_plasma_window_management *,
                        wl_array *) {}
void onManagerNoopVoid(void *, org_kde_plasma_window_management *) {}

const org_kde_plasma_window_management_listener &managerListener() {
  static const org_kde_plasma_window_management_listener listener = [] {
    org_kde_plasma_window_management_listener l{};
    l.show_desktop_changed = onManagerNoop;
    l.window = onManagerNoop; // deprecated internal-id form
    l.stacking_order_changed = onManagerNoopArray;
    l.stacking_order_uuid_changed = onManagerNoopStr;
    l.window_with_uuid = onWindowWithUuid;
    l.stacking_order_changed_2 = onManagerNoopVoid;
    return l;
  }();
  return listener;
}

void onRegistryGlobal(void *data, wl_registry *registry, uint32_t name,
                      const char *interface, uint32_t version) {
  auto *p = static_cast<BackendPrivate *>(data);
  if (!p || p->management || !interface)
    return;
  if (std::strcmp(interface, org_kde_plasma_window_management_interface.name) !=
      0)
    return;
  // The protocol says the global must be bound at version 17 or later; older
  // servers have no stacking-order request, so there is nothing to list.
  const uint32_t wanted =
      std::min<uint32_t>(version, org_kde_plasma_window_management_interface.version);
  p->management = static_cast<org_kde_plasma_window_management *>(
      wl_registry_bind(registry, name, &org_kde_plasma_window_management_interface,
                       wanted));
  p->version = wanted;
  if (p->management)
    org_kde_plasma_window_management_add_listener(p->management,
                                                  &managerListener(), p);
}

void onRegistryGlobalRemove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener &registryListener() {
  static const wl_registry_listener listener = [] {
    wl_registry_listener l{};
    l.global = onRegistryGlobal;
    l.global_remove = onRegistryGlobalRemove;
    return l;
  }();
  return listener;
}

} // namespace

void KWinProtocolBackend::Private::attach(const QString &uuid) {
  if (!active || uuid.isEmpty() || byUuid.contains(uuid))
    return;
  const quintptr handle = model.ensure(uuid);
  if (!handle)
    return;
  org_kde_plasma_window *window =
      org_kde_plasma_window_management_get_window_by_uuid(
          management, uuid.toUtf8().constData());
  if (!window) {
    model.remove(handle);
    return;
  }
  auto *data = new WindowData{this, handle, false};
  byUuid.insert(uuid, handle);
  windows.insert(handle, window);
  windowData.append(data);
  org_kde_plasma_window_add_listener(window, &windowListener(), data);
}

void KWinProtocolBackend::Private::schedule() {
  if (emitScheduled || !q)
    return;
  emitScheduled = true;
  // Coalesce a burst (initial batch, geometry storm while dragging a window)
  // into one snapshot for the monitor.
  QTimer::singleShot(0, q, [this] {
    emitScheduled = false;
    if (active)
      emitSnapshot();
  });
}

void KWinProtocolBackend::Private::emitSnapshot() {
  if (!q)
    return;
  const int count = model.size();
  if (count != lastLoggedCount) {
    lastLoggedCount = count;
    qInfo() << "KWinProtocolBackend:" << count << "windows";
  }
  hasSnapshot = true;
  emit q->windowsUpdated();
}

void KWinProtocolBackend::Private::flush() {
  if (!display || !active)
    return;
  if (wl_display_flush(display) == 0) {
    if (writeNotifier)
      writeNotifier->setEnabled(false);
  } else if (errno == EAGAIN) {
    if (writeNotifier)
      writeNotifier->setEnabled(true);
  }
}

void KWinProtocolBackend::Private::dispatch() {
  if (!display || !active)
    return;
  while (wl_display_prepare_read(display) != 0) {
    if (wl_display_dispatch_pending(display) < 0) {
      fail(QStringLiteral("dispatch failed: %1")
               .arg(QString::fromLocal8Bit(std::strerror(errno))));
      return;
    }
  }
  if (wl_display_flush(display) < 0 && errno != EAGAIN) {
    wl_display_cancel_read(display);
    fail(QStringLiteral("flush failed: %1")
             .arg(QString::fromLocal8Bit(std::strerror(errno))));
    return;
  }
  if (wl_display_read_events(display) < 0) {
    fail(QStringLiteral("read failed: %1")
             .arg(QString::fromLocal8Bit(std::strerror(errno))));
    return;
  }
  if (wl_display_dispatch_pending(display) < 0) {
    fail(QStringLiteral("pending dispatch failed: %1")
             .arg(QString::fromLocal8Bit(std::strerror(errno))));
    return;
  }
  flush();
}

void KWinProtocolBackend::Private::fail(const QString &reason) {
  if (!active && !display)
    return;
  active = false;
  if (readNotifier)
    readNotifier->setEnabled(false);
  if (writeNotifier)
    writeNotifier->setEnabled(false);
  qWarning() << "KWinProtocolBackend:" << reason;
  // Drop the stale snapshot: detection, the picker and capture restoration
  // must not keep treating windows we can no longer see as live.
  model = PlasmaToplevelModel{};
  byUuid.clear();
  windows.clear();
  if (q)
    emit q->windowsUpdated();
}

void KWinProtocolBackend::Private::destroyAll() {
  active = false;
  if (readNotifier) {
    readNotifier->setEnabled(false);
    readNotifier->deleteLater();
    readNotifier = nullptr;
  }
  if (writeNotifier) {
    writeNotifier->setEnabled(false);
    writeNotifier->deleteLater();
    writeNotifier = nullptr;
  }
  // No per-window destroy requests: the compositor drops those objects when a
  // window closes, and wl_display_disconnect frees anything left over.
  qDeleteAll(windowData);
  windowData.clear();
  windows.clear();
  byUuid.clear();
  model = PlasmaToplevelModel{};
  hasSnapshot = false;
  emitScheduled = false;
  lastLoggedCount = -1;
  if (registry) {
    wl_registry_destroy(registry);
    registry = nullptr;
  }
  if (display) {
    wl_display_flush(display);
    wl_display_disconnect(display);
    display = nullptr;
  }
  management = nullptr;
  version = 0;
}

#else // !HAVE_WAYLAND_TOPLEVEL

class KWinProtocolBackend::Private {
public:
  bool active = false;
  bool hasSnapshot = false;
  int version = 0;
};

#endif

KWinProtocolBackend::KWinProtocolBackend(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>()) {
#ifdef HAVE_WAYLAND_TOPLEVEL
  d->q = this;
#endif
}

KWinProtocolBackend::~KWinProtocolBackend() { stop(); }

bool KWinProtocolBackend::isAvailable() {
#ifdef HAVE_WAYLAND_TOPLEVEL
  return !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
         !qEnvironmentVariableIsEmpty("WAYLAND_SOCKET");
#else
  return false;
#endif
}

bool KWinProtocolBackend::start(QString *error) {
#ifdef HAVE_WAYLAND_TOPLEVEL
  if (d->display && d->active)
    return true;
  if (!isAvailable()) {
    if (error)
      *error = QStringLiteral("No Wayland session in the environment");
    return false;
  }
  d->display = wl_display_connect(nullptr);
  if (!d->display) {
    if (error)
      *error = QStringLiteral("Could not connect to the Wayland display");
    return false;
  }
  d->registry = wl_display_get_registry(d->display);
  wl_registry_add_listener(d->registry, &registryListener(), d.get());
  if (wl_display_roundtrip(d->display) < 0 || !d->management) {
    d->destroyAll();
    if (error)
      *error = QStringLiteral(
          "Compositor does not expose org_kde_plasma_window_management");
    return false;
  }
  if (d->version < 17) {
    const quint32 version = d->version;
    d->destroyAll();
    if (error)
      *error = QStringLiteral(
                   "org_kde_plasma_window_management is too old (v%1)")
                   .arg(version);
    return false;
  }

  d->active = true;
  if (auto *order = org_kde_plasma_window_management_get_stacking_order(
          d->management)) {
    org_kde_plasma_stacking_order_add_listener(order, &stackingListener(),
                                               d.get());
  }
  if (wl_display_roundtrip(d->display) < 0) {
    d->destroyAll();
    if (error)
      *error = QStringLiteral("Compositor window list request failed");
    return false;
  }
  // The stacking-order reply only gives uuids; each one was turned into a
  // get_window_by_uuid request during that dispatch. One more roundtrip
  // delivers those initial batches, so the first snapshot the monitor sees is
  // the real window list instead of an empty one.
  if (wl_display_roundtrip(d->display) < 0) {
    d->destroyAll();
    if (error)
      *error = QStringLiteral("Compositor window details request failed");
    return false;
  }

  const int fd = wl_display_get_fd(d->display);
  d->readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
  d->writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
  d->writeNotifier->setEnabled(false);
  connect(d->readNotifier, &QSocketNotifier::activated, this,
          [this] { d->dispatch(); });
  connect(d->writeNotifier, &QSocketNotifier::activated, this,
          [this] { d->flush(); });
  d->flush();
  qInfo() << "KWinProtocolBackend: bound org_kde_plasma_window_management v"
          << d->version << "with" << d->model.size() << "windows - no script";
  return true;
#else
  if (error)
    *error = QStringLiteral("Built without Wayland client support");
  return false;
#endif
}

void KWinProtocolBackend::stop() {
#ifdef HAVE_WAYLAND_TOPLEVEL
  d->destroyAll();
#endif
}

bool KWinProtocolBackend::isActive() const { return d->active; }

bool KWinProtocolBackend::hasSnapshot() const { return d->hasSnapshot; }

QList<WindowInfo> KWinProtocolBackend::windows() const {
#ifdef HAVE_WAYLAND_TOPLEVEL
  return d->model.snapshot(&outputSizeAt);
#else
  return {};
#endif
}

int KWinProtocolBackend::protocolVersion() const { return int(d->version); }

} // namespace TrophyBackend
