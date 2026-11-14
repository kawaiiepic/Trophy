/*
 * Trophy - Unit Tests
 * GameIdentity evidence providers: launcher ancestry and desktop entries.
 *
 * Both are provenance signals for a process that no reporter described.
 * They are deliberately strict: a launcher must be a games-only launcher
 * (steam/lutris/wine/…), and a desktop entry must declare Categories=Game.
 */
#include "detection/GameIdentity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

using TrophyBackend::GameIdentity;

namespace {

// Wait for the /proc children list to publish the launcher's child. Falls
// back to a test-only /proc scan if CONFIG_PROC_CHILDREN is unavailable.
qint64 firstChildOf(qint64 parent, int timeoutMs = 3000) {
  const QString path = QString("/proc/%1/task/%1/children").arg(parent);
  const auto scan = [parent]() -> qint64 {
    QDir proc("/proc");
    for (const QString &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      bool ok = false;
      const qint64 pid = entry.toLongLong(&ok);
      if (!ok)
        continue;
      QFile stat(QString("/proc/%1/stat").arg(pid));
      if (!stat.open(QIODevice::ReadOnly))
        continue;
      const QByteArray bytes = stat.read(8192);
      const qsizetype close = bytes.lastIndexOf(')');
      if (close < 0 || close + 2 >= bytes.size())
        continue;
      const QList<QByteArray> fields = bytes.mid(close + 2).split(' ');
      if (fields.size() >= 2 && fields.at(1).toLongLong() == parent)
        return pid;
    }
    return 0;
  };
  for (int waited = 0; waited < timeoutMs; waited += 25) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
      const QByteArray contents = file.read(4096).trimmed();
      if (!contents.isEmpty()) {
        bool ok = false;
        const qint64 pid = contents.split(' ').first().toLongLong(&ok);
        if (ok && pid > 0)
          return pid;
      }
    }
    QThread::msleep(25);
  }
  return scan();
}

void writeDesktopEntry(const QString &directory, const QString &id,
                       const QString &name, const QString &categories) {
  QDir().mkpath(directory);
  QFile file(directory + "/" + id + ".desktop");
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\n"
                            "Categories=%2\n")
                 .arg(name, categories)
                 .toUtf8());
}

class GameIdentityTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(temp.isValid());
    savedDataHome = qgetenv("XDG_DATA_HOME");
    savedDataDirs = qgetenv("XDG_DATA_DIRS");
    home1 = temp.path() + "/home1/applications";
    home2 = temp.path() + "/home2/applications";
    qputenv("XDG_DATA_HOME", (temp.path() + "/home1").toUtf8());
    qputenv("XDG_DATA_DIRS", (temp.path() + "/home2").toUtf8());
  }
  void TearDown() override {
    if (savedDataHome.isNull())
      qunsetenv("XDG_DATA_HOME");
    else
      qputenv("XDG_DATA_HOME", savedDataHome);
    if (savedDataDirs.isNull())
      qunsetenv("XDG_DATA_DIRS");
    else
      qputenv("XDG_DATA_DIRS", savedDataDirs);
  }

  QTemporaryDir temp;
  QByteArray savedDataHome;
  QByteArray savedDataDirs;
  QString home1;
  QString home2;
};

} // namespace

TEST(GameIdentityLauncherTest, RecognizesGamesOnlyLaunchers) {
  EXPECT_TRUE(GameIdentity::isLauncherName("steam"));
  EXPECT_TRUE(GameIdentity::isLauncherName("lutris"));
  EXPECT_TRUE(GameIdentity::isLauncherName("Heroic"));
  EXPECT_TRUE(GameIdentity::isLauncherName("wine64-preloader"));
  EXPECT_TRUE(GameIdentity::isLauncherName("gamescope"));
  EXPECT_TRUE(GameIdentity::isLauncherName("/usr/bin/pressure-vessel-wrap"));
  EXPECT_FALSE(GameIdentity::isLauncherName("sleep"));
  EXPECT_FALSE(GameIdentity::isLauncherName("mpv"));
  EXPECT_FALSE(GameIdentity::isLauncherName("firefox"));
}

TEST_F(GameIdentityTest, LauncherAncestryFindsRenamedLauncherProcess) {
  // A games-only launcher running this process makes it plausible content:
  // the same evidence a graphics reporter used to provide.
  const QString launcherPath = temp.path() + "/lutris";
  ASSERT_TRUE(QFile::copy("/bin/sh", launcherPath));
  ASSERT_TRUE(QFile(launcherPath).setPermissions(
      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  QProcess launcher;
  // Not a single command: sh would exec it and lose the launcher name.
  launcher.start(launcherPath, {"-c", "sleep 30; :"});
  ASSERT_TRUE(launcher.waitForStarted(5000));
  const qint64 child = firstChildOf(launcher.processId());
  ASSERT_GT(child, 0);

  const auto provenance = GameIdentity::launcherAncestry(child);
  EXPECT_TRUE(provenance.launcher);
  EXPECT_EQ(provenance.launcherName, "lutris");

  // The launcher itself is not its own ancestor.
  EXPECT_FALSE(GameIdentity::launcherAncestry(launcher.processId()).launcher);

  launcher.kill();
  launcher.waitForFinished(5000);
}

TEST_F(GameIdentityTest, PlainProcessHasNoLauncherProvenance) {
  QProcess child;
  child.start("sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted(5000));
  const auto provenance = GameIdentity::launcherAncestry(child.processId());
  EXPECT_FALSE(provenance.launcher);
  child.kill();
  child.waitForFinished(5000);
}

TEST_F(GameIdentityTest, DesktopEntryGameCategoryIsProvenance) {
  writeDesktopEntry(home1, "org.example.Game", "Example Game", "Game;ActionGame;");
  const auto info = GameIdentity::desktopEntryFor("org.example.Game");
  ASSERT_TRUE(info.found);
  EXPECT_TRUE(info.isGame);
  EXPECT_EQ(info.name, QString("Example Game"));
}

TEST_F(GameIdentityTest, DesktopEntryMatchingIsCaseInsensitive) {
  // WM_CLASS is frequently lowercased by the compositor.
  writeDesktopEntry(home1, "org.example.Game", "Example Game", "Game;");
  const auto info = GameIdentity::desktopEntryFor("org.example.game");
  ASSERT_TRUE(info.found);
  EXPECT_TRUE(info.isGame);
}

TEST_F(GameIdentityTest, DesktopEntrySearchesEveryDataDirectory) {
  writeDesktopEntry(home2, "org.example.Second", "Second", "Game;");
  const auto info = GameIdentity::desktopEntryFor("org.example.Second");
  ASSERT_TRUE(info.found);
  EXPECT_TRUE(info.isGame);
}

TEST_F(GameIdentityTest, DesktopEntryAcceptsExplicitSuffix) {
  writeDesktopEntry(home1, "org.example.Game", "Example Game", "Game;");
  const auto info = GameIdentity::desktopEntryFor("org.example.Game.desktop");
  EXPECT_TRUE(info.found);
  EXPECT_TRUE(info.isGame);
}

TEST_F(GameIdentityTest, PlayerAndBrowserEntriesAreNotProvenance) {
  writeDesktopEntry(home1, "mpv", "mpv", "AudioVideo;Video;Player;");
  writeDesktopEntry(home1, "firefox", "Firefox", "Network;WebBrowser;");
  EXPECT_TRUE(GameIdentity::desktopEntryFor("mpv").found);
  EXPECT_FALSE(GameIdentity::desktopEntryFor("mpv").isGame);
  EXPECT_TRUE(GameIdentity::desktopEntryFor("firefox").found);
  EXPECT_FALSE(GameIdentity::desktopEntryFor("firefox").isGame);
}

TEST_F(GameIdentityTest, UnknownAppIdIsNotProvenance) {
  const auto info = GameIdentity::desktopEntryFor("com.example.Missing");
  EXPECT_FALSE(info.found);
  EXPECT_FALSE(info.isGame);
}
