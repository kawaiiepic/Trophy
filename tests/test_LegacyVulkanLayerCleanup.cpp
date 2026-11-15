/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/LegacyVulkanLayerCleanup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using TrophyBackend::LegacyVulkanLayerCleanup::manifestPath;
using TrophyBackend::LegacyVulkanLayerCleanup::removeOwnedRegistration;

namespace {

void ensureApplication() {
  if (!QCoreApplication::instance()) {
    static int argc = 1;
    static char arg0[] = "test";
    static char *argv[] = {arg0, nullptr};
    new QCoreApplication(argc, argv);
  }
}

class LegacyVulkanLayerCleanupTest : public ::testing::Test {
protected:
  QTemporaryDir data;
  QByteArray oldDataHome;

  void SetUp() override {
    ensureApplication();
    ASSERT_TRUE(data.isValid());
    oldDataHome = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    ASSERT_TRUE(QDir().mkpath(QFileInfo(manifestPath()).absolutePath()));
    reporters = data.path() + "/trophy/reporters";
    ASSERT_TRUE(QDir().mkpath(reporters));
  }

  void TearDown() override {
    if (oldDataHome.isNull())
      qunsetenv("XDG_DATA_HOME");
    else
      qputenv("XDG_DATA_HOME", oldDataHome);
  }

  void writeManifest(const QByteArray &content) {
    QFile file(manifestPath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(content), content.size());
  }

  void writeDurable(const QString &name) {
    QFile file(reporters + "/" + name);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("elf"), 3);
  }

  QString reporters;
};

} // namespace

TEST_F(LegacyVulkanLayerCleanupTest, RemovesOwnedManifest) {
  writeManifest("{\"file_format_version\":\"1.0.0\",\"layer\":{\"name\":"
                "\"VK_LAYER_TROPHY_detection\"}}");
  ASSERT_TRUE(removeOwnedRegistration());
  EXPECT_FALSE(QFile::exists(manifestPath()));
}

TEST_F(LegacyVulkanLayerCleanupTest, LeavesForeignManifestAlone) {
  const QByteArray foreign =
      "{\"file_format_version\":\"1.0.0\",\"layer\":{\"name\":"
      "\"VK_LAYER_someone_else\"}}";
  writeManifest(foreign);
  ASSERT_TRUE(removeOwnedRegistration());
  ASSERT_TRUE(QFile::exists(manifestPath()));
  QFile file(manifestPath());
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  EXPECT_EQ(file.readAll(), foreign);
}

TEST_F(LegacyVulkanLayerCleanupTest, MissingManifestIsANoOp) {
  QFile::remove(manifestPath());
  EXPECT_TRUE(removeOwnedRegistration());
}

TEST_F(LegacyVulkanLayerCleanupTest, RemovesDurableCopiesButKeepsForeignFiles) {
  writeManifest("{\"layer\":{\"name\":\"VK_LAYER_TROPHY_detection\"}}");
  writeDurable("libtrophy_layer-abc123.so");
  writeDurable("libtrophy_layer32-def456.so");
  writeDurable("libtrophy_gl_shim-old.so");
  ASSERT_TRUE(removeOwnedRegistration());
  EXPECT_FALSE(QFile::exists(reporters + "/libtrophy_layer-abc123.so"));
  EXPECT_FALSE(QFile::exists(reporters + "/libtrophy_layer32-def456.so"));
  EXPECT_TRUE(QFile::exists(reporters + "/libtrophy_gl_shim-old.so"));
}
