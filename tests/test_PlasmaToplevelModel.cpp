/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/PlasmaToplevelModel.h"

#include <gtest/gtest.h>

using TrophyBackend::PlasmaToplevelModel;
using TrophyBackend::WindowInfo;

namespace {

struct Recorded {
  PlasmaToplevelModel model;
};

QList<WindowInfo> rowsFor(PlasmaToplevelModel &model) {
  return model.snapshot();
}

} // namespace

TEST(PlasmaToplevelModelTest, DuplicateUuidReusesTheSameRecord) {
  // KWin announces a window twice: once in the stacking-order reply and once
  // as a window_with_uuid event. One window must stay one row.
  PlasmaToplevelModel model;
  const quintptr first = model.ensure("uuid-a");
  const quintptr second = model.ensure("uuid-a");
  ASSERT_NE(first, 0u);
  EXPECT_EQ(first, second);
  EXPECT_EQ(model.size(), 1);

  model.setAppId(first, "game");
  model.announce(first);
  const auto rows = rowsFor(model);
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].appId, "game");
}

TEST(PlasmaToplevelModelTest, EmptyUuidIsIgnored) {
  PlasmaToplevelModel model;
  EXPECT_EQ(model.ensure(QString()), 0u);
  EXPECT_EQ(model.size(), 0);
}

TEST(PlasmaToplevelModelTest, WindowIsNotPublishedBeforeInitialState) {
  // Events arrive one at a time; publishing a half-filled window would put an
  // unidentified row in detection, the picker and capture restoration.
  PlasmaToplevelModel model;
  const quintptr handle = model.ensure("uuid-a");
  model.setAppId(handle, "game");
  model.setTitle(handle, "Game");
  EXPECT_TRUE(rowsFor(model).isEmpty());

  model.announce(handle);
  const auto rows = rowsFor(model);
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].title, "Game");
}

TEST(PlasmaToplevelModelTest, StateFlagsBecomeFocusFullscreenAndMinimized) {
  PlasmaToplevelModel model;
  const quintptr handle = model.ensure("uuid-a");
  model.announce(handle);

  model.setState(handle, PlasmaToplevelModel::StateActive |
                             PlasmaToplevelModel::StateFullscreen);
  auto rows = rowsFor(model);
  ASSERT_EQ(rows.size(), 1);
  EXPECT_TRUE(rows[0].focused);
  EXPECT_TRUE(rows[0].fullscreen);
  EXPECT_FALSE(rows[0].minimized);

  // Active is cleared when another window takes focus; fullscreen stays.
  model.setState(handle, PlasmaToplevelModel::StateFullscreen |
                             PlasmaToplevelModel::StateMinimized);
  rows = rowsFor(model);
  EXPECT_FALSE(rows[0].focused);
  EXPECT_TRUE(rows[0].fullscreen);
  EXPECT_TRUE(rows[0].minimized);

  // Maximized is not fullscreen: borderless detection must not borrow it.
  model.setState(handle, PlasmaToplevelModel::StateMaximized);
  rows = rowsFor(model);
  EXPECT_FALSE(rows[0].fullscreen);
}

TEST(PlasmaToplevelModelTest, ClientGeometryWinsOverFrameGeometry) {
  // Frame geometry includes decorations, so a later frame update must not
  // overwrite the client area once the compositor reported it.
  PlasmaToplevelModel model;
  const quintptr handle = model.ensure("uuid-a");
  model.setGeometry(handle, 10, 20, 100, 200);
  model.setClientGeometry(handle, 11, 21, 90, 190);
  model.setGeometry(handle, 10, 20, 100, 200);
  model.announce(handle);

  const auto rows = rowsFor(model);
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].x, 11);
  EXPECT_EQ(rows[0].y, 21);
  EXPECT_EQ(rows[0].width, 90);
  EXPECT_EQ(rows[0].height, 190);
}

TEST(PlasmaToplevelModelTest, ResourceNameFillsMissingAppId) {
  // XWayland windows may report a WM_CLASS but no app_id. Without the
  // fallback they would be anonymous rows no desktop entry can match.
  PlasmaToplevelModel model;
  const quintptr handle = model.ensure("uuid-a");
  model.setResourceName(handle, "Wine");
  model.announce(handle);
  ASSERT_EQ(rowsFor(model).size(), 1);
  EXPECT_EQ(rowsFor(model)[0].appId, "Wine");

  model.setAppId(handle, "steam_app_1");
  EXPECT_EQ(rowsFor(model)[0].appId, "steam_app_1");
}

TEST(PlasmaToplevelModelTest, PidAndStableKeyCrossOver) {
  // The key must be the same uuid the KWin script reports as internalId so
  // monitor state survives a backend switch; the numeric id stays 0 and the
  // monitor derives a stable synthetic id from the key.
  PlasmaToplevelModel model;
  const quintptr handle = model.ensure("{uuid-1}");
  model.setPid(handle, 4242);
  model.announce(handle);

  const auto rows = rowsFor(model);
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].pid, 4242);
  EXPECT_EQ(rows[0].key, QStringLiteral("{uuid-1}"));
  EXPECT_EQ(rows[0].id, 0u);
}

TEST(PlasmaToplevelModelTest, RemoveDropsTheWindowAndIgnoresLateEvents) {
  PlasmaToplevelModel model;
  const quintptr handle = model.ensure("uuid-a");
  model.setAppId(handle, "game");
  model.announce(handle);
  ASSERT_EQ(rowsFor(model).size(), 1);

  model.remove(handle);
  EXPECT_TRUE(rowsFor(model).isEmpty());
  EXPECT_FALSE(model.contains(handle));

  // Pending events for a closed window must not resurrect it.
  model.setAppId(handle, "late");
  model.announce(handle);
  EXPECT_TRUE(rowsFor(model).isEmpty());

  // The same uuid may come back as a new window.
  const quintptr again = model.ensure("uuid-a");
  ASSERT_NE(again, 0u);
  EXPECT_NE(again, handle);
  model.announce(again);
  EXPECT_EQ(rowsFor(model).size(), 1);
}

TEST(PlasmaToplevelModelTest, SnapshotOrderIsStableAcrossUpdates) {
  PlasmaToplevelModel model;
  const quintptr second = model.ensure("uuid-b");
  const quintptr first = model.ensure("uuid-a");
  model.announce(first);
  model.announce(second);

  auto rows = rowsFor(model);
  ASSERT_EQ(rows.size(), 2);
  EXPECT_EQ(rows[0].key, QStringLiteral("uuid-a"));
  EXPECT_EQ(rows[1].key, QStringLiteral("uuid-b"));

  // A later update to the other window must not reorder the snapshot: the
  // monitor's synthetic ids are key-derived, but tests and consumers still
  // deserve deterministic output.
  model.setTitle(second, "updated");
  rows = rowsFor(model);
  EXPECT_EQ(rows[0].key, QStringLiteral("uuid-a"));
  EXPECT_EQ(rows[1].key, QStringLiteral("uuid-b"));
}

TEST(PlasmaToplevelModelTest, OutputSizeResolverFillsCoverageInputs) {
  // The protocol gives window geometry but not the output layout; Qt supplies
  // the screen, which restores borderless-coverage scoring on KDE.
  PlasmaToplevelModel model;
  const quintptr handle = model.ensure("uuid-a");
  model.setGeometry(handle, 100, 50, 1920, 1080);
  model.announce(handle);

  const auto rows = model.snapshot(
      [](int x, int y) { return (x == 100 && y == 50) ? QSize(1920, 1080) : QSize(); });
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].outputWidth, 1920);
  EXPECT_EQ(rows[0].outputHeight, 1080);

  const auto unknown = model.snapshot([](int, int) { return QSize(); });
  ASSERT_EQ(unknown.size(), 1);
  EXPECT_EQ(unknown[0].outputWidth, 0);
  EXPECT_EQ(unknown[0].outputHeight, 0);
}
