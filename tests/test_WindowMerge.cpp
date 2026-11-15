/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/WindowMerge.h"

#include <gtest/gtest.h>

using TrophyBackend::mergeWindowSnapshots;
using TrophyBackend::WindowInfo;

namespace {

WindowInfo window(const QString &appId, const QString &title, qint64 pid = 0) {
  WindowInfo info;
  info.key = appId + QLatin1Char(':') + title;
  info.appId = appId;
  info.title = title;
  info.pid = pid;
  return info;
}

} // namespace

TEST(WindowMergeTest, WithoutEnrichmentThePrimaryListIsUnchanged) {
  const QList<WindowInfo> primary{window("game", "Game", 42)};
  EXPECT_EQ(mergeWindowSnapshots(primary, {}), primary);
}

TEST(WindowMergeTest, EnrichmentAloneIsPublished) {
  // No compositor backend at all (unknown desktop): XWayland rows are the
  // only window list, which is what lets Proton games detect there.
  const QList<WindowInfo> extra{window("game", "Game", 42)};
  const auto merged = mergeWindowSnapshots({}, extra);
  ASSERT_EQ(merged.size(), 1);
  EXPECT_EQ(merged[0].pid, 42);
}

TEST(WindowMergeTest, PidIsFilledIntoThePrimaryRowAndIdsAreKept) {
  WindowInfo primary = window("game", "Game");
  primary.key = "protocol-uuid";
  primary.id = 0x80000001ULL;
  WindowInfo extra = window("Game", "Game", 4242);
  extra.pid = 4242;
  extra.width = 1920;
  extra.height = 1080;
  extra.outputWidth = 2560;
  extra.outputHeight = 1440;

  const auto merged = mergeWindowSnapshots({primary}, {extra});
  ASSERT_EQ(merged.size(), 1);
  // The compositor row survives: its id/key keep monitor state stable.
  EXPECT_EQ(merged[0].key, "protocol-uuid");
  EXPECT_EQ(merged[0].id, 0x80000001ULL);
  EXPECT_EQ(merged[0].pid, 4242);
  EXPECT_EQ(merged[0].width, 1920);
  EXPECT_EQ(merged[0].outputWidth, 2560);
}

TEST(WindowMergeTest, ComparisonIgnoresAppIdCaseButNotDifferentApps) {
  const auto merged = mergeWindowSnapshots({window("Game", "Game")},
                                           {window("game", "Game", 7)});
  ASSERT_EQ(merged.size(), 1);
  EXPECT_EQ(merged[0].pid, 7);

  // Same title, different app: a different window.
  const auto distinct = mergeWindowSnapshots({window("alpha", "Shared")},
                                             {window("beta", "Shared", 9)});
  EXPECT_EQ(distinct.size(), 2);
}

TEST(WindowMergeTest, TitlesMustBeCompatible) {
  // Same app, two documents: the title keeps them apart.
  const auto merged = mergeWindowSnapshots({window("editor", "one.txt")},
                                           {window("editor", "two.txt", 5)});
  EXPECT_EQ(merged.size(), 2);
}

TEST(WindowMergeTest, SameAppAndGeometryIsTheSameWindowEvenWithDifferentTitles) {
  WindowInfo primary = window("game", "Game - Level 1");
  primary.x = 10;
  primary.y = 20;
  primary.width = 1920;
  primary.height = 1080;
  WindowInfo extra = window("game", "Game", 77);
  extra.x = 10;
  extra.y = 20;
  extra.width = 1920;
  extra.height = 1080;

  const auto merged = mergeWindowSnapshots({primary}, {extra});
  ASSERT_EQ(merged.size(), 1);
  EXPECT_EQ(merged[0].pid, 77);
}

TEST(WindowMergeTest, ExistingPidWinsOverTheSupplement) {
  const auto merged = mergeWindowSnapshots({window("game", "Game", 11)},
                                           {window("game", "Game", 22)});
  ASSERT_EQ(merged.size(), 1);
  EXPECT_EQ(merged[0].pid, 11);
}

TEST(WindowMergeTest, FocusIsAdoptedOnlyWhenThePrimaryHasNone) {
  WindowInfo extra = window("game", "Game", 5);
  extra.focused = true;
  auto merged = mergeWindowSnapshots({window("game", "Game")}, {extra});
  ASSERT_EQ(merged.size(), 1);
  EXPECT_TRUE(merged[0].focused);

  // A primary that already reports focus keeps its answer, and an
  // unrelated enrichable window cannot claim focus too.
  WindowInfo primaryFocused = window("browser", "Docs", 3);
  primaryFocused.focused = true;
  merged = mergeWindowSnapshots({primaryFocused}, {extra});
  ASSERT_EQ(merged.size(), 2);
  EXPECT_TRUE(merged[0].focused);
  EXPECT_FALSE(merged[1].focused);
}

TEST(WindowMergeTest, AppendedRowsAreDeduplicatedByKey) {
  WindowInfo extra = window("game", "Game", 5);
  extra.key = "x11:0x1";
  const auto merged = mergeWindowSnapshots({}, {extra, extra});
  EXPECT_EQ(merged.size(), 1);
}
