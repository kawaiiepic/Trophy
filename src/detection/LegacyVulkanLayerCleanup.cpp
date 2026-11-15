/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LegacyVulkanLayerCleanup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace TrophyBackend::LegacyVulkanLayerCleanup {

namespace {

QString reportersDirectory() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/trophy/reporters");
}

constexpr const char *kLayerName = "VK_LAYER_TROPHY_detection";
constexpr const char *kLibName = "libtrophy_layer";

bool isOwnedManifest(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  const QByteArray bytes = file.read(1024 * 1024);
  if (file.error() != QFileDevice::NoError)
    return false;
  return bytes.contains(kLayerName) || bytes.contains(kLibName);
}

} // namespace

QString manifestPath() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/vulkan/implicit_layer.d/trophy_layer.json");
}

bool removeOwnedRegistration() {
  bool ok = true;
  const QString manifest = manifestPath();
  if (QFileInfo::exists(manifest)) {
    if (isOwnedManifest(manifest))
      ok = QFile::remove(manifest) && ok;
    // A foreign file at our name is deliberately left alone.
    const QString dir = QFileInfo(manifest).absolutePath();
    QDir().rmdir(dir); // no-op unless the directory is now empty
  }

  // Content-addressed copies created by enrollment (libtrophy_layer-<sha>.so,
  // libtrophy_layer32-<sha>.so). The prefix copy under lib/trophy is not in
  // this directory and is left for the package manager / install prefix.
  const QDir reporters(reportersDirectory());
  for (const QString &pattern :
       {QStringLiteral("libtrophy_layer-*.so"),
        QStringLiteral("libtrophy_layer32-*.so")}) {
    for (const QFileInfo &entry :
         reporters.entryInfoList({pattern}, QDir::Files | QDir::NoSymLinks)) {
      if (!QFile::remove(entry.filePath()))
        ok = false;
    }
  }
  return ok;
}

} // namespace TrophyBackend::LegacyVulkanLayerCleanup
