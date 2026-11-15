/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/WlrToplevelModel.h"

#include <gtest/gtest.h>

using TrophyBackend::WindowInfo;
using TrophyBackend::WlrToplevelModel;

TEST(WlrToplevelModelTest, ToplevelsAreNotPublishedUntilDone) {
  // The protocol streams title/app_id/state per window and closes the batch
  // with `done`; publishing early would show a half-identified window.
  WlrToplevelModel model;
  const quintptr handle = model.add();
  ASSERT_TRUE(model.contains(handle));
  model.setAppId(handle, "game");
  model.setTitle(handle, "Game");
  EXPECT_TRUE(model.snapshot().isEmpty());

  model.commit(handle);
  const auto rows = model.snapshot();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].appId, "game");
  EXPECT_EQ(rows[0].title, "Game");
}

TEST(WlrToplevelModelTest, StateIndicesBecomeFocusFullscreenAndMinimized) {
  WlrToplevelModel model;
  const quintptr handle = model.add();
  model.setAppId(handle, "game");
  model.setStateIndices(handle, {WlrToplevelModel::StateActivated,
                                WlrToplevelModel::StateFullscreen});
  model.commit(handle);

  auto rows = model.snapshot();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_TRUE(rows[0].focused);
  EXPECT_TRUE(rows[0].fullscreen);
  EXPECT_FALSE(rows[0].minimized);

  // State is a replacement, not an accumulation: activation moving to
  // another window clears ours.
  model.setStateIndices(handle, {WlrToplevelModel::StateMinimized});
  rows = model.snapshot();
  EXPECT_FALSE(rows[0].focused);
  EXPECT_FALSE(rows[0].fullscreen);
  EXPECT_TRUE(rows[0].minimized);

  model.setStateIndices(handle, {});
  rows = model.snapshot();
  EXPECT_FALSE(rows[0].focused);
  EXPECT_FALSE(rows[0].minimized);
}

TEST(WlrToplevelModelTest, UnknownStateValuesAreIgnored) {
  WlrToplevelModel model;
  const quintptr handle = model.add();
  model.setStateIndices(handle, {99, 4, 3});
  model.commit(handle);
  const auto rows = model.snapshot();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_TRUE(rows[0].fullscreen);
  EXPECT_FALSE(rows[0].focused);
}

TEST(WlrToplevelModelTest, RowsHaveNoPidAndANamespacedKey) {
  // wlr-foreign-toplevel-management carries no process id: detection must
  // rely on identity evidence, or on an enrichment source.
  WlrToplevelModel model;
  const quintptr handle = model.add();
  model.setAppId(handle, "game");
  model.commit(handle);
  const auto rows = model.snapshot();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].pid, 0);
  EXPECT_EQ(rows[0].key, QStringLiteral("wlr:%1").arg(handle));
  EXPECT_EQ(rows[0].id, 0u);
}

TEST(WlrToplevelModelTest, ClosedToplevelsDisappear) {
  WlrToplevelModel model;
  const quintptr first = model.add();
  const quintptr second = model.add();
  model.setStateIndices(first, {WlrToplevelModel::StateActivated});
  model.commit(first);
  model.commit(second);
  ASSERT_EQ(model.snapshot().size(), 2);

  model.remove(first);
  EXPECT_FALSE(model.contains(first));
  const auto rows = model.snapshot();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].key, QStringLiteral("wlr:%1").arg(second));
}

TEST(WlrToplevelModelTest, SnapshotOrderIsStable) {
  WlrToplevelModel model;
  const quintptr second = model.add();
  const quintptr first = model.add();
  model.commit(first);
  model.commit(second);
  const auto rows = model.snapshot();
  ASSERT_EQ(rows.size(), 2);
  EXPECT_LT(rows[0].key, rows[1].key);
}
