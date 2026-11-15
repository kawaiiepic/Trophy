/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WlrToplevelBackend.h"

#include "WlrToplevelModel.h"

#include <QDebug>
#include <QHash>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>

#ifdef HAVE_WAYLAND_TOPLEVEL
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#endif

namespace TrophyBackend {

#ifdef HAVE_WAYLAND_TOPLEVEL

class WlrToplevelBackend::Private {
public:
  struct HandleData {
    Private *owner = nullptr;
    quintptr handle = 0;
  };

  WlrToplevelBackend *q = nullptr;
  WlrToplevelModel model;
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  zwlr_foreign_toplevel_manager_v1 *manager = nullptr;
  QSocketNotifier *readNotifier = nullptr;
  QSocketNotifier *writeNotifier = nullptr;
  QHash<quintptr, zwlr_foreign_toplevel_handle_v1 *> handles;
  QList<HandleData *> handleData;
  bool active = false;
  bool hasSnapshot = false;
  bool emitScheduled = false;

  void schedule();
  void emitSnapshot();
  void dispatch();
  void flush();
  void fail(const QString &reason);
  void destroyAll();
};

namespace {

using BackendPrivate = WlrToplevelBackend::Private;
using HandleData = WlrToplevelBackend::Private::HandleData;

HandleData *dataFor(void *data) {
  auto *h = static_cast<HandleData *>(data);
  return (h && h->owner) ? h : nullptr;
}

void onTitle(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *title) {
  auto *h = dataFor(data);
  if (!h)
    return;
  h->owner->model.setTitle(h->handle,
                           QString::fromUtf8(title ? title : ""));
  h->owner->schedule();
}

void onAppId(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *appId) {
  auto *h = dataFor(data);
  if (!h)
    return;
  h->owner->model.setAppId(h->handle,
                           QString::fromUtf8(appId ? appId : ""));
  h->owner->schedule();
}

void onState(void *data, zwlr_foreign_toplevel_handle_v1 *, wl_array *state) {
  auto *h = dataFor(data);
  if (!h || !state)
    return;
  QList<quint32> indices;
  const auto count = state->size / sizeof(quint32);
  const auto *values = static_cast<const quint32 *>(state->data);
  for (size_t i = 0; i < count; ++i)
    indices.append(values[i]);
  h->owner->model.setStateIndices(h->handle, indices);
  h->owner->schedule();
}

void onDone(void *data, zwlr_foreign_toplevel_handle_v1 *) {
  auto *h = dataFor(data);
  if (!h)
    return;
  h->owner->model.commit(h->handle);
  h->owner->schedule();
}

void onClosed(void *data, zwlr_foreign_toplevel_handle_v1 *toplevel) {
  auto *h = dataFor(data);
  if (!h)
    return;
  BackendPrivate *owner = h->owner;
  owner->model.remove(h->handle);
  owner->handles.remove(h->handle);
  owner->handleData.removeOne(h);
  zwlr_foreign_toplevel_handle_v1_destroy(toplevel);
  delete h;
  owner->schedule();
}

void onNoop(void *, zwlr_foreign_toplevel_handle_v1 *) {}
void onNoopOutput(void *, zwlr_foreign_toplevel_handle_v1 *, wl_output *) {}
void onNoopParent(void *, zwlr_foreign_toplevel_handle_v1 *,
                  zwlr_foreign_toplevel_handle_v1 *) {}

const zwlr_foreign_toplevel_handle_v1_listener &handleListener() {
  static const zwlr_foreign_toplevel_handle_v1_listener listener = [] {
    zwlr_foreign_toplevel_handle_v1_listener l{};
    l.title = onTitle;
    l.app_id = onAppId;
    l.output_enter = onNoopOutput;
    l.output_leave = onNoopOutput;
    l.state = onState;
    l.done = onDone;
    l.closed = onClosed;
    l.parent = onNoopParent;
    return l;
  }();
  return listener;
}

void onToplevel(void *data, zwlr_foreign_toplevel_manager_v1 *,
                zwlr_foreign_toplevel_handle_v1 *toplevel) {
  auto *p = static_cast<BackendPrivate *>(data);
  if (!p || !toplevel)
    return;
  const quintptr handle = p->model.add();
  auto *h = new HandleData{p, handle};
  p->handles.insert(handle, toplevel);
  p->handleData.append(h);
  zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &handleListener(), h);
}

void onFinished(void *data, zwlr_foreign_toplevel_manager_v1 *manager) {
  auto *p = static_cast<BackendPrivate *>(data);
  zwlr_foreign_toplevel_manager_v1_destroy(manager);
  if (p) {
    p->manager = nullptr;
    p->fail(QStringLiteral("compositor stopped the toplevel list"));
  }
}

const zwlr_foreign_toplevel_manager_v1_listener &managerListener() {
  static const zwlr_foreign_toplevel_manager_v1_listener listener = [] {
    zwlr_foreign_toplevel_manager_v1_listener l{};
    l.toplevel = onToplevel;
    l.finished = onFinished;
    return l;
  }();
  return listener;
}

void onRegistryGlobal(void *data, wl_registry *registry, uint32_t name,
                      const char *interface, uint32_t version) {
  auto *p = static_cast<BackendPrivate *>(data);
  if (!p || p->manager || !interface)
    return;
  if (std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) !=
      0)
    return;
  // Version 2 added the fullscreen state; bind it when offered.
  const uint32_t wanted = version < 2 ? version : 2;
  p->manager = static_cast<zwlr_foreign_toplevel_manager_v1 *>(wl_registry_bind(
      registry, name, &zwlr_foreign_toplevel_manager_v1_interface, wanted));
  if (p->manager)
    zwlr_foreign_toplevel_manager_v1_add_listener(p->manager,
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

void WlrToplevelBackend::Private::schedule() {
  if (emitScheduled || !q)
    return;
  emitScheduled = true;
  QTimer::singleShot(0, q, [this] {
    emitScheduled = false;
    if (active)
      emitSnapshot();
  });
}

void WlrToplevelBackend::Private::emitSnapshot() {
  if (!q)
    return;
  hasSnapshot = true;
  emit q->windowsUpdated();
}

void WlrToplevelBackend::Private::flush() {
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

void WlrToplevelBackend::Private::dispatch() {
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

void WlrToplevelBackend::Private::fail(const QString &reason) {
  if (!active && !display)
    return;
  active = false;
  if (readNotifier)
    readNotifier->setEnabled(false);
  if (writeNotifier)
    writeNotifier->setEnabled(false);
  qWarning() << "WlrToplevelBackend:" << reason;
  model = WlrToplevelModel{};
  if (q)
    emit q->windowsUpdated();
}

void WlrToplevelBackend::Private::destroyAll() {
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
  qDeleteAll(handleData);
  handleData.clear();
  handles.clear();
  model = WlrToplevelModel{};
  hasSnapshot = false;
  emitScheduled = false;
  if (registry) {
    wl_registry_destroy(registry);
    registry = nullptr;
  }
  if (display) {
    wl_display_flush(display);
    wl_display_disconnect(display);
    display = nullptr;
  }
  manager = nullptr;
}

#else // !HAVE_WAYLAND_TOPLEVEL

class WlrToplevelBackend::Private {
public:
  bool active = false;
  bool hasSnapshot = false;
};

#endif

WlrToplevelBackend::WlrToplevelBackend(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>()) {
#ifdef HAVE_WAYLAND_TOPLEVEL
  d->q = this;
#endif
}

WlrToplevelBackend::~WlrToplevelBackend() { stop(); }

bool WlrToplevelBackend::isAvailable() {
#ifdef HAVE_WAYLAND_TOPLEVEL
  return !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
         !qEnvironmentVariableIsEmpty("WAYLAND_SOCKET");
#else
  return false;
#endif
}

bool WlrToplevelBackend::start(QString *error) {
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
  if (wl_display_roundtrip(d->display) < 0 || !d->manager) {
    d->destroyAll();
    if (error)
      *error = QStringLiteral(
          "Compositor does not expose wlr-foreign-toplevel-management");
    return false;
  }
  d->active = true;
  // The manager sends every existing toplevel during this roundtrip, and
  // each handle's initial batch before its `done`.
  if (wl_display_roundtrip(d->display) < 0) {
    d->destroyAll();
    if (error)
      *error = QStringLiteral("Compositor toplevel list request failed");
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
  qInfo() << "WlrToplevelBackend: bound wlr-foreign-toplevel-management with"
          << d->model.size() << "toplevels";
  d->schedule();
  return true;
#else
  if (error)
    *error = QStringLiteral("Built without Wayland client support");
  return false;
#endif
}

void WlrToplevelBackend::stop() {
#ifdef HAVE_WAYLAND_TOPLEVEL
  d->destroyAll();
#endif
}

bool WlrToplevelBackend::isActive() const { return d->active; }

bool WlrToplevelBackend::hasSnapshot() const { return d->hasSnapshot; }

QList<WindowInfo> WlrToplevelBackend::windows() const {
#ifdef HAVE_WAYLAND_TOPLEVEL
  return d->model.snapshot();
#else
  return {};
#endif
}

} // namespace TrophyBackend
