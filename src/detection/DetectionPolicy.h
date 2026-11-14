/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace TrophyBackend {

/**
 * @brief Everything the detection decision is allowed to know.
 *
 * Built once per event from an identity plus at most one window snapshot.
 * Keeping the decision a pure function of this struct means the reporter
 * path, the window reconciler and the pending-candidate recheck cannot
 * drift apart (they used to encode the same rules separately).
 */
struct DetectionEvidence {
  bool excluded = false;   // blacklist/veto applies to key or display name
  bool whitelisted = false; // explicit user rule matches
  bool steam = false;       // SteamAppId (wire/env) or a steamapps path
  // Provenance: why this process is plausible game content at all. Each of
  // these is independent evidence; none of them is a window property.
  bool reporter = false;    // a graphics reporter reported this PID
  bool launcher = false;    // a known game launcher is in the process ancestry
  bool desktopGame = false; // toplevel app id maps to a Categories=Game entry
  bool focused = false;     // window state for the PID (when known)
  bool fullscreen = false;  // compositor fullscreen flag
  bool usableWindow = false; // known geometry at least 64x64 (windowed games)
  bool minimized = false;

  /** Any provenance signal at all (reporter, launcher, desktop entry). */
  bool provenance() const { return reporter || launcher || desktopGame; }
};

enum class DetectionDecision {
  Ignore,   // not a game, or not enough evidence to even ask the user
  Candidate, // provenance only: wait for focus/fullscreen
  Detect,   // confident enough to report/auto-capture
};

/**
 * @brief One evidence threshold for every detection source.
 *
 * Rules, in order:
 *  - veto always wins;
 *  - explicit whitelist or Steam identity detects immediately;
 *  - provenance (reporter, launcher ancestry, game desktop entry) detects
 *    once its window is focused and capturable: fullscreen, or a window of
 *    at least 64x64 (windowed games are games too), otherwise it waits as
 *    a candidate;
 *  - a window alone is never enough: windows without provenance need
 *    whitelist or Steam evidence, which is what keeps fullscreen video
 *    players, browsers and other apps out of the detection path.
 */
class DetectionPolicy {
public:
  static DetectionDecision decide(const DetectionEvidence &evidence);
};

} // namespace TrophyBackend
