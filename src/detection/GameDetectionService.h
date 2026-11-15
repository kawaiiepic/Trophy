/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAbstractItemModel>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <memory>

#include "GameListModel.h"
#include "GameIdentity.h"

class RecordingManagerTest;

namespace TrophyBackend {

class GameWhitelist;
class WaylandWindowMonitor;
class PipeWireStreamWatcher;
struct WindowInfo;
struct AudioStreamInfo;
class GameDetectionServiceTest; // Forward declaration for unit testing

class GameDetectionService : public QObject {
  Q_OBJECT

  // Property system for UI/QML synchronization
  Q_PROPERTY(
      QString currentGameName READ currentGameName NOTIFY currentGameChanged)
  Q_PROPERTY(bool isGameRunning READ isGameRunning NOTIFY currentGameChanged)
  Q_PROPERTY(
      qint64 currentGamePid READ currentGamePid NOTIFY currentGameChanged)
  Q_PROPERTY(QStringList recentGames READ recentGames NOTIFY recentGamesChanged)
  // QML-facing unified games list (live + history + user lists,
  // filtered by gameFilterText; built-in exclusions hide unless
  // showDefaultExclusions is set).
  Q_PROPERTY(QAbstractItemModel *gamesModel READ gamesModel CONSTANT)
  Q_PROPERTY(QString gameFilterText READ gameFilterText WRITE setGameFilterText
                 NOTIFY gameFilterTextChanged)
  Q_PROPERTY(bool showDefaultExclusions READ showDefaultExclusions WRITE
                 setShowDefaultExclusions NOTIFY showDefaultExclusionsChanged)

public:
  explicit GameDetectionService(QObject *parent = nullptr);
  ~GameDetectionService();

  void start();
  void stop();

  // Getters for properties
  bool isGameRunning() const {
    // A listing-only protocol can track a game without a process id.
    return !m_currentGameName.isEmpty();
  }
  QString currentGameName() const { return m_currentGameName; }
  qint64 currentGamePid() const { return m_currentGamePid; }
  // Raw match key for the live game (reporter class / steam_app_<id>).
  QString currentGameKey() const { return m_currentGameKey; }
  // Cached detector evidence only; distinct from portal stream metadata.
  // Capture owners pin this snapshot, never borrowing a later namesake.
  QVariantMap captureIdentityInfo(const QString &gameName) const;
  QString captureKeyForGame(const QString &name) const {
    return name == m_currentGameName && !m_currentGameKey.isEmpty()
               ? m_currentGameKey : name;
  }
  QStringList recentGames() const { return m_recentGames; }

  /**
   * @brief Whether the compositor currently shows a capturable window for
   * the game.
   *
   * Portal restore data references concrete windows (KDE matches by appId +
   * title), so firing a capture request before any game window exists
   * guarantees a picker even with a valid token. Application waits for
   * this before starting a token-armed capture.
   *
   * Windowed games count: any non-minimized window of the tracked identity
   * at least 64x64 (or with unknown geometry) qualifies. Small clicker-style
   * windows are legitimate capture targets, not only fullscreen/borderless
   * surfaces. A launcher/splash surface that gets replaced is handled by the
   * recording manager's hand-off retry instead of a size gate here.
   */
  Q_INVOKABLE bool hasVisibleWindow() const;
  /** Whether a window row is the tracked game (by PID or by its identity). */
  bool windowBelongsToTrackedGame(const WindowInfo &window) const;
  /**
   * @brief Whether any non-minimized compositor window exists for the game.
   *
   * Slightly weaker than hasVisibleWindow(): even a degenerate-sized window
   * qualifies. Application uses this to decide when it is worth asking the
   * user to authorize a new game; starting capture still requires a sane
   * window (hasVisibleWindow). Falls back to false without a monitor or a
   * tracked game.
   */
  bool hasGameWindow() const;
  // One-line snapshot of the tracked identity's compositor rows (pid,
  // geometry, fullscreen/minimized state, output coverage) for diagnosing
  // why a window-gated decision deferred. Always descriptive, never empty.
  QString describeTrackedGameWindows() const;
  // Cached compositor evidence for a gameplay-sized (fullscreen or
  // output-covering) window of this stable identity, used to pin the captured
  // process and to keep a surviving launcher from faking a game exit.
  // Includes minimized windows: minimizing is not process exit. Windowed
  // games have no such window, so callers fall back to process liveness.
  QList<qint64> gameplayWindowPids(const QString &key) const;
  /**
   * @brief Detect games that were already running when Trophy started.
   * Shims only report at graphics init, so a pre-existing game never
   * sends START. Runs automatically on the first window snapshot after
   * start(), and is public for explicit calls. Only focused (or fullscreen)
   * windows with whitelist or Steam evidence are reported — same bar as
   * live detection, never a guess.
   */
  void reconcileRunningGames();

  // Whitelist/Blacklist Management (invokable for the Games page;
  // true when the list actually changed, for UI feedback)
  Q_INVOKABLE bool addToWhitelist(const QString &windowClass,
                                  const QString &displayName);
  Q_INVOKABLE bool addToBlacklist(const QString &windowClass);
  Q_INVOKABLE bool removeFromWhitelist(const QString &windowClass);
  Q_INVOKABLE bool removeFromBlacklist(const QString &windowClass);
  // Whitelist a Steam game by AppID: stored under the steam_app_<id> key
  // form the matcher already understands (window appIds, shim names).
  Q_INVOKABLE bool addSteamGame(uint appId, const QString &displayName);
  // Atomic list moves for the Games page: one mutation, one signal, one
  // model reset. Separate remove+add calls from QML straddle a reset that
  // tears down the invoking delegate mid-handler and loses the second
  // half (seen live: Allow on a system row removed it but never re-added
  // it anywhere).
  Q_INVOKABLE bool moveToWhitelist(const QString &key, const QString &display);
  Q_INVOKABLE bool moveToBlacklist(const QString &key);
  // Drop the block veto for a key (Prompt pole). Whitelist membership is
  // orthogonal and untouched: poles govern capture behavior, the x button
  // governs existence. True when a veto was actually cleared.
  Q_INVOKABLE bool unblockGame(const QString &key);
  // True when the key or name carries a block veto (normalized either
  // way). Application suppresses the authorize prompt for blocked games.
  Q_INVOKABLE bool isBlocked(const QString &keyOrName) const;
  // Manual entry from the Add dialog: records the key as user-known (so
  // it stays visible with no rule attached) plus the chosen list.
  Q_INVOKABLE bool addCustomGame(const QString &key, const QString &display,
                                 bool block);
  // Full deletion: drops whitelist/blacklist entries, user-known mark,
  // and history. Built-ins fall back to their neutral marker row.
  Q_INVOKABLE bool forgetGame(const QString &key);
  QStringList whitelist() const;
  QStringList blacklist() const;

  QAbstractItemModel *gamesModel() const { return m_gamesFilter; }
  // Presentation-only lookup for older recordings that carry a title, not
  // an app identity. Ambiguous titles must not borrow another game's icon.
  Q_INVOKABLE QUrl iconSourceForGame(const QString &name) const;
  QString gameFilterText() const;
  void setGameFilterText(const QString &text);
  // Portal restore-token games (silent capture). Pushed by Application
  // from SettingsManager; approval is derived, never stored here.
  void setTokenGames(const QStringList &games);
  bool showDefaultExclusions() const;
  void setShowDefaultExclusions(bool show);

  /**
   * @brief Attach the compositor window monitor for confidence fusion.
   * Without one, only whitelisted / Steam-identified shim events detect.
   */
  void setWindowMonitor(WaylandWindowMonitor *monitor);

  /**
   * @brief Attach the desktop-agnostic PipeWire stream signal.
   *
   * A playback stream gives a PID and binary name on any compositor. It is
   * evidence only when the process is a Steam title or an explicit rule
   * matches; arbitrary audio apps are ignored.
   */
  void setStreamWatcher(PipeWireStreamWatcher *watcher);

  // SteamAppId/SteamGameId from a process environment, usable for any PID
  // after the fact.
  static uint steamAppIdForPid(qint64 pid);

  // "steam_app_1422450" -> 1422450, anything else -> 0. Window classes
  // from Steam games carry the id; resolving it gives display names.
  static uint steamAppIdFromAppId(const QString &appId);

  /**
   * @brief Whether a process currently exists (signal 0 probe).
   *
   * EPERM counts as alive (process exists but is not signalable).
   * Pure predicate so tests can cover it without timers.
   */
  static bool processExists(qint64 pid);

signals:
  void gameDetected(const QString &gameName, qint64 pid);
  void gameClosed(const QString &gameName);
  /**
   * @brief A focused window nobody has evidence for.
   *
   * Discovery hint: the window is focused and not degenerate (windowed games
   * count — size must not gate discovery, clicker-style titles are real
   * games), but has no reporter/launcher/desktop-entry/Steam evidence, so
   * detection stays out of it. Application offers to add it to the Games
   * page — never starts a capture. Already rate limited here: one per window
   * key per launch, a small launch total and a spacing floor.
   */
  void undiscoveredGameSeen(const QString &appId, const QString &title,
                            qint64 pid);
  void currentGameChanged();
  void recentGamesChanged();
  void whitelistChanged();
  void blacklistChanged();
  void gameFilterTextChanged();
  void showDefaultExclusionsChanged();
  void tokenGamesChanged();

private slots:
  void onNewConnection();
  void onSocketReadyRead();
  void onSocketDisconnected();
  void onMonitorWindowsChanged();
  void onLivenessTimeout();

private:
  // Grant the test class access to private members (fixes your build errors)
  friend class GameDetectionServiceTest;
  friend class ApplicationAuthorizePromptTest;
  friend class ::RecordingManagerTest;

  // Internal identification helpers
  QString getSteamAppName(uint appId);
  // Identity resolution itself lives in GameIdentity (shared by the socket
  // handler, the window reconciler and future detection sources); this
  // alias keeps the service's existing signatures readable.
  using ReporterIdentity = GameIdentity::ReporterIdentity;
  // Pure resolver, also used by daemon-free fixtures. Inputs come only
  // from the PID named in a START event, never a process scan.
  static ReporterIdentity resolveReporterIdentity(const QString &reporter,
                                                  const QString &exe,
                                                  const QStringList &argv,
                                                  uint steamAppId,
                                                  const QString &winePrefix = {});
  ReporterIdentity reporterIdentity(qint64 pid, const QString &reporter,
                                    uint steamAppId);

  /**
   * @brief Decide whether a shim event is confident enough to report.
   * Blacklist is checked by the caller. Instant detect on: whitelist
   * hit, SteamAppId present, or the exe living under a steamapps library.
   * Otherwise the focused window for the PID decides: fullscreen, or any
   * window of at least 64x64. Windowed games are games too — many
   * clicker/puzzle titles never cover the output — so size is no longer a
   * fullscreen requirement. Unknown (0x0) geometry needs the fullscreen flag.
   */
  bool isConfidentGame(qint64 pid, const QString &gameName, uint steamAppId);
  bool isBlacklistedIdentity(const QString &rawKey,
                             const QString &displayName) const;
  bool processLooksLikeSteamGame(qint64 pid);
  // Discovery hint: a focused window with no evidence at all. Never
  // starts anything; rate limits live in the members below.
  void hintUndetectedGame();
  bool isUndetectedGameCandidate(const WindowInfo &w);
  // Evidence providers (result + monotonic ms at check); cached because
  // windowsChanged fires for every focus/title change and these read /proc.
  GameIdentity::LaunchProvenance launcherProvenance(qint64 pid);
  GameIdentity::DesktopEntryInfo desktopEntry(const QString &appId);
  void onAudioStreamAppeared(const AudioStreamInfo &stream);

  void trackGame(qint64 pid, const QString &gameName,
                 const QString &rawKey = QString());
  void recheckCandidates();
  // A veto landing on the live game retires it (dashboard back to IDLE,
  // gameClosed for clean capture teardown). Called by every path that
  // adds a veto; no-op unless the canonical key matches the tracked game.
  void retireBlockedGame(const QString &canonicalKey);

  /**
   * @brief Borderless-windowed heuristic: a window covering >= 90% of its
   * output counts like fullscreen (many games never set the flag). Used to
   * recognise gameplay-sized windows for PID pinning and exit classification,
   * not to gate capture anymore.
   */
  static bool coversOutput(const WindowInfo &w);

  void updateCurrentGame(const QString &gameName, qint64 pid,
                         const QString &rawKey = QString());
  /**
   * @brief Failover target, preferring focus, same identity, then visibility.
   * PID order breaks ties. Only reported PIDs are probed; never a scan.
   */
  qint64 lowestLiveTrackedPid() const;
  /**
   * @brief Failover after the tracked PID went away (STOP / disconnect /
   * liveness). Promotes the best still-running tracked PID; when none
   * is alive, drops the proven-dead entries (so a later START for the
   * same name detects fresh) and goes idle.
   */
  void promoteNextTrackedGame();
  void addToRecentGames(const QString &gameName, const QString &rawKey);

  bool m_running = false;
  QString m_currentGameName;
  qint64 m_currentGamePid = 0;
  // Consecutive liveness ticks with no window for a PID-less game.
  int m_windowMissCount = 0;
  // Stable match key: steam_app_<id>, executable path, Wine prefix +
  // game path, or a legacy reporter key when executable info is absent.
  QString m_currentGameKey;
  QStringList m_recentGames;
  // Raw keys parallel to m_recentGames (same order, capped together).
  QStringList m_recentKeys;
  // Restore-token keys (stable identities, or legacy display names).
  QSet<QString> m_tokenGames;

  QLocalServer *m_server;
  QMap<QLocalSocket *, qint64> m_activeConnections;
  QMap<qint64, QString> m_pidToGameName;
  // Raw match keys parallel to m_pidToGameName (cleaned at the same sites).
  QMap<qint64, QString> m_pidToRawKey;
  // Liveness watchdog for the tracked game (see onLivenessTimeout): a
  // single targeted kill(pid, 0) every few seconds — not a scan.
  QTimer *m_livenessTimer = nullptr;
  // Shim reporters that did not (yet) pass the confidence gate. Pruned
  // on STOP/disconnect; re-checked whenever window state changes so a
  // windowless launch detects once its (windowed or fullscreen) window
  // appears and takes focus.
  QMap<qint64, QString> m_pendingCandidates;
  // Reporter/window identity parallel to m_pendingCandidates. A Steam
  // resolved display name is not necessarily the key stored by the user.
  QMap<qint64, QString> m_pendingRawKeys;
  // Steam manifest name cache: appId -> display name. Validity is keyed
  // on the steamapps library path + libraryfolders.vdf mtime and the
  // matched manifest mtime, so library/name changes refresh automatically.
  // Only positive hits are cached: a missing manifest may appear later
  // without the vdf mtime changing (old-format vdfs).
  struct SteamNameCacheEntry {
    QString name;
    QString manifestPath;
    QDateTime modified;
  };
  QMap<uint, SteamNameCacheEntry> m_steamNameCache;
  QString m_steamLibCacheKey;
  // Per-PID looks-like-Steam verdicts (see processLooksLikeSteamGame):
  // pid -> (result, monotonic ms at check). Member-scoped so instances
  // (and tests) never share verdicts.
  QMap<qint64, QPair<bool, qint64>> m_steamCheckCache;
  // Per-PID launcher ancestry and per-app-id desktop entries. Same member
  // scoping and TTL rules as the Steam verdict cache above.
  struct LauncherCacheEntry {
    GameIdentity::LaunchProvenance provenance;
    qint64 checkedAt = 0;
  };
  QMap<qint64, LauncherCacheEntry> m_launcherCache;
  struct DesktopCacheEntry {
    GameIdentity::DesktopEntryInfo info;
    qint64 checkedAt = 0;
  };
  QMap<QString, DesktopCacheEntry> m_desktopCache;
  static constexpr qint64 kCacheTtlMs = 5000;
  // Desktop entries change only on install/remove and a miss scans every
  // applications directory, so negatives are cached longer than PIDs.
  static constexpr qint64 kDesktopCacheTtlMs = 60000;
  static constexpr int kCacheCap = 256;
  QElapsedTimer m_uptimeClock;

  // Discovery-hint bookkeeping (see undiscoveredGameSeen): keys already
  // offered this launch, how many were offered, and when the last one was.
  QSet<QString> m_hintedKeys;
  // First moment each candidate window was seen. A transient dialog can be
  // focused and sane for a moment, so an offer waits for the window to stay
  // put; entries are dropped as soon as the window disappears.
  QHash<QString, qint64> m_hintFirstSeen;
  qint64 m_hintMinAgeMs = 4000;
  int m_hintCount = 0;
  qint64 m_lastHintAt = -1;
  // Tuned for a human: a couple per launch, spaced out. Tests lower these
  // through the friend seam.
  int m_hintMax = 3;
  qint64 m_hintSpacingMs = 20000;

  WaylandWindowMonitor *m_monitor = nullptr;
  PipeWireStreamWatcher *m_streamWatcher = nullptr;
  std::unique_ptr<GameWhitelist> m_whitelist;

  // Games-page list model (owned here so the pointer stays stable).
  std::unique_ptr<GameListModel> m_gamesModel;
  GameListFilterModel *m_gamesFilter = nullptr;
  void rebuildModels();
};

} // namespace TrophyBackend
