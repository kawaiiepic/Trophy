/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "XWaylandWindowBackend.h"

#include "WindowOutputSize.h"

#include <QDebug>
#include <QMutexLocker>
#include <QSet>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef HAVE_XCB
#include <xcb/xcb.h>
#endif

namespace TrophyBackend {

namespace {

/** Lowercased WM_CLASS class field, matching how compositors pick app_id. */
QString normalizedClass(const QString &wmClass) {
  return wmClass.trimmed().toLower();
}

} // namespace

QList<WindowInfo> XWaylandWindowBackend::parseWindows(
    const QList<X11WindowRecord> &records, quint32 activeId,
    const X11Atoms &atoms) {
  QList<WindowInfo> out;
  out.reserve(records.size());
  for (const X11WindowRecord &record : records) {
    if (!record.viewable)
      continue;
    if (atoms.skipTaskbar != 0 && record.state.contains(atoms.skipTaskbar))
      continue; // docks, panels, tooltips: not capturable game windows
    const QString appId = normalizedClass(record.wmClass);
    if (appId.isEmpty())
      continue;

    WindowInfo info;
    // The monitor derives a stable synthetic id from the key, so X11 ids
    // cannot collide with a compositor's own id space.
    info.key = QStringLiteral("x11:0x%1").arg(record.id, 0, 16);
    info.appId = appId;
    info.title = record.title;
    info.pid = record.pid;
    info.focused = activeId != 0 && record.id == activeId;
    info.fullscreen =
        atoms.fullscreen != 0 && record.state.contains(atoms.fullscreen);
    info.minimized = atoms.hidden != 0 && record.state.contains(atoms.hidden);
    info.x = record.x;
    info.y = record.y;
    info.width = record.width;
    info.height = record.height;
    if (record.width > 0 && record.height > 0) {
      const QSize output = outputSizeAtPoint(record.x + record.width / 2,
                                            record.y + record.height / 2);
      if (output.isValid() && output.width() > 0 && output.height() > 0) {
        info.outputWidth = output.width();
        info.outputHeight = output.height();
      }
    }
    out.append(info);
  }
  std::sort(out.begin(), out.end(),
            [](const WindowInfo &a, const WindowInfo &b) {
              return a.key < b.key;
            });
  return out;
}

QString XWaylandWindowBackend::signatureFor(const QList<WindowInfo> &windows) {
  QString sig;
  sig.reserve(windows.size() * 64);
  for (const WindowInfo &w : windows) {
    sig += w.key;
    sig += '|';
    sig += w.appId;
    sig += '|';
    sig += w.title;
    sig += '|';
    sig += QString::number(w.pid);
    sig += '|';
    sig += w.focused ? '1' : '0';
    sig += w.fullscreen ? '1' : '0';
    sig += w.minimized ? '1' : '0';
    sig += '|';
    sig += QString::number(w.x);
    sig += ',';
    sig += QString::number(w.y);
    sig += ',';
    sig += QString::number(w.width);
    sig += 'x';
    sig += QString::number(w.height);
    sig += '\n';
  }
  return sig;
}

XWaylandWindowBackend::XWaylandWindowBackend(QObject *parent)
    : QObject(parent) {}

bool XWaylandWindowBackend::isAvailable() {
  return !qEnvironmentVariableIsEmpty("DISPLAY");
}

#ifdef HAVE_XCB

namespace {

struct Atoms {
  xcb_atom_t clientList = XCB_ATOM_NONE;
  xcb_atom_t activeWindow = XCB_ATOM_NONE;
  xcb_atom_t pid = XCB_ATOM_NONE;
  xcb_atom_t netName = XCB_ATOM_NONE;
  xcb_atom_t wmName = XCB_ATOM_NONE;
  xcb_atom_t wmClass = XCB_ATOM_NONE;
  xcb_atom_t wmState = XCB_ATOM_NONE;
  xcb_atom_t utf8String = XCB_ATOM_NONE;
  X11Atoms ewmh;
};

xcb_atom_t internAtom(xcb_connection_t *conn, const char *name) {
  const xcb_intern_atom_cookie_t cookie =
      xcb_intern_atom(conn, 0, static_cast<uint16_t>(std::strlen(name)), name);
  xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, nullptr);
  if (!reply)
    return XCB_ATOM_NONE;
  const xcb_atom_t atom = reply->atom;
  std::free(reply);
  return atom;
}

QByteArray propertyBytes(xcb_connection_t *conn, xcb_window_t window,
                         xcb_atom_t property, xcb_atom_t type,
                         quint32 maxItems) {
  if (property == XCB_ATOM_NONE)
    return {};
  const xcb_get_property_cookie_t cookie = xcb_get_property(
      conn, 0, window, property, type, 0, maxItems);
  xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, nullptr);
  if (!reply)
    return {};
  QByteArray bytes;
  const int length = xcb_get_property_value_length(reply);
  if (length > 0)
    bytes = QByteArray(static_cast<const char *>(xcb_get_property_value(reply)),
                       length);
  std::free(reply);
  return bytes;
}

QList<quint32> atomList(const QByteArray &bytes) {
  QList<quint32> atoms;
  const auto count = bytes.size() / int(sizeof(quint32));
  const auto *data = reinterpret_cast<const quint32 *>(bytes.constData());
  atoms.reserve(count);
  for (int i = 0; i < count; ++i)
    atoms.append(data[i]);
  return atoms;
}

quint32 cardinal(const QByteArray &bytes) {
  if (bytes.size() < int(sizeof(quint32)))
    return 0;
  quint32 value = 0;
  std::memcpy(&value, bytes.constData(), sizeof(value));
  return value;
}

/** First NUL-terminated string of a WM_CLASS property (instance, class). */
QString wmClassField(const QByteArray &bytes, int field) {
  int start = 0;
  for (int i = 0; i <= field; ++i) {
    const int end = bytes.indexOf('\0', start);
    const QByteArray piece =
        end < 0 ? bytes.mid(start) : bytes.mid(start, end - start);
    if (i == field)
      return QString::fromLatin1(piece);
    if (end < 0)
      return {};
    start = end + 1;
  }
  return {};
}

QList<X11WindowRecord> readWindows(xcb_connection_t *conn, xcb_window_t root,
                                   const Atoms &atoms, quint32 *activeId) {
  QList<X11WindowRecord> records;
  const QByteArray listBytes = propertyBytes(conn, root, atoms.clientList,
                                             XCB_ATOM_WINDOW, 4096);
  *activeId = cardinal(propertyBytes(conn, root, atoms.activeWindow,
                                     XCB_ATOM_WINDOW, 1));

  for (const quint32 id : atomList(listBytes)) {
    if (id == 0)
      continue;
    X11WindowRecord record;
    record.id = id;

    const xcb_get_window_attributes_cookie_t attrCookie =
        xcb_get_window_attributes(conn, id);
    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(conn, attrCookie, nullptr);
    if (!attr)
      continue;
    record.viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
    std::free(attr);

    record.pid = cardinal(propertyBytes(conn, id, atoms.pid, XCB_ATOM_CARDINAL, 1));
    record.wmClass =
        wmClassField(propertyBytes(conn, id, atoms.wmClass, XCB_ATOM_STRING, 64), 1);
    QString title = QString::fromUtf8(
        propertyBytes(conn, id, atoms.netName, atoms.utf8String, 4096));
    if (title.isEmpty()) {
      title = QString::fromLatin1(
          propertyBytes(conn, id, atoms.wmName, XCB_ATOM_STRING, 4096));
    }
    record.title = title.trimmed();
    record.state =
        atomList(propertyBytes(conn, id, atoms.wmState, XCB_ATOM_ATOM, 64));

    const xcb_get_geometry_cookie_t geomCookie = xcb_get_geometry(conn, id);
    xcb_get_geometry_reply_t *geom =
        xcb_get_geometry_reply(conn, geomCookie, nullptr);
    if (geom) {
      const xcb_translate_coordinates_cookie_t translateCookie =
          xcb_translate_coordinates(conn, id, root, 0, 0);
      xcb_translate_coordinates_reply_t *translated =
          xcb_translate_coordinates_reply(conn, translateCookie, nullptr);
      if (translated) {
        record.x = translated->dst_x;
        record.y = translated->dst_y;
        std::free(translated);
      }
      record.width = geom->width;
      record.height = geom->height;
      std::free(geom);
    }
    records.append(record);
  }
  return records;
}

Atoms resolveAtoms(xcb_connection_t *conn) {
  Atoms atoms;
  atoms.clientList = internAtom(conn, "_NET_CLIENT_LIST");
  atoms.activeWindow = internAtom(conn, "_NET_ACTIVE_WINDOW");
  atoms.pid = internAtom(conn, "_NET_WM_PID");
  atoms.netName = internAtom(conn, "_NET_WM_NAME");
  atoms.wmName = internAtom(conn, "WM_NAME");
  atoms.wmClass = internAtom(conn, "WM_CLASS");
  atoms.wmState = internAtom(conn, "_NET_WM_STATE");
  atoms.utf8String = internAtom(conn, "UTF8_STRING");
  atoms.ewmh.fullscreen = internAtom(conn, "_NET_WM_STATE_FULLSCREEN");
  atoms.ewmh.hidden = internAtom(conn, "_NET_WM_STATE_HIDDEN");
  atoms.ewmh.skipTaskbar = internAtom(conn, "_NET_WM_STATE_SKIP_TASKBAR");
  return atoms;
}

xcb_window_t rootWindow(xcb_connection_t *conn) {
  const xcb_setup_t *setup = xcb_get_setup(conn);
  if (!setup)
    return 0;
  xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
  return screens.data ? screens.data->root : 0;
}

} // namespace

bool XWaylandWindowBackend::start() {
  if (!isAvailable()) {
    qInfo() << "XWayland windows unavailable: no DISPLAY";
    return false;
  }
  xcb_connection_t *conn = xcb_connect(nullptr, nullptr);
  if (!conn || xcb_connection_has_error(conn)) {
    if (conn)
      xcb_disconnect(conn);
    qInfo() << "XWayland windows unavailable: cannot connect to the X display";
    return false;
  }
  const Atoms atoms = resolveAtoms(conn);
  const bool usable = atoms.clientList != XCB_ATOM_NONE && rootWindow(conn) != 0;
  xcb_disconnect(conn);
  if (!usable) {
    qInfo() << "XWayland windows unavailable: no _NET_CLIENT_LIST";
    return false;
  }
  m_active.store(true);
  qInfo() << "XWayland window tracking armed (X11 _NET_WM_PID)";
  return true;
}

void XWaylandWindowBackend::refresh() {
  if (!m_active.load())
    return;
  xcb_connection_t *conn = xcb_connect(nullptr, nullptr);
  if (!conn || xcb_connection_has_error(conn)) {
    if (conn)
      xcb_disconnect(conn);
    return;
  }
  const Atoms atoms = resolveAtoms(conn);
  const xcb_window_t root = rootWindow(conn);
  QList<WindowInfo> windows;
  if (atoms.clientList != XCB_ATOM_NONE && root != 0) {
    quint32 activeId = 0;
    const QList<X11WindowRecord> records =
        readWindows(conn, root, atoms, &activeId);
    windows = parseWindows(records, activeId, atoms.ewmh);
  }
  xcb_disconnect(conn);

  m_hasSnapshot.store(true);
  const QString signature = signatureFor(windows);
  {
    QMutexLocker lock(&m_mutex);
    if (signature == m_lastSignature)
      return;
    m_lastSignature = signature;
    m_windows = windows;
  }
  emit windowsUpdated();
}

#else // !HAVE_XCB

bool XWaylandWindowBackend::start() { return false; }

void XWaylandWindowBackend::refresh() {}

#endif

void XWaylandWindowBackend::stop() {
  m_active.store(false);
  QMutexLocker lock(&m_mutex);
  m_lastSignature.clear();
  m_windows.clear();
}

QList<WindowInfo> XWaylandWindowBackend::windows() const {
  QMutexLocker lock(&m_mutex);
  return m_windows;
}

} // namespace TrophyBackend
