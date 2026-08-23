using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using WorldAtWarVR.Launcher.Core;

namespace WorldAtWarVR.Launcher.Core.Tests;

[TestClass]
public sealed class LauncherCoreTests
{
    [TestMethod]
    public void NormalizeDirectory_PreservesDriveRoot()
    {
        Assert.AreEqual(
            @"E:\",
            GameInstallationValidator.NormalizeDirectory(@"E:\"));
        Assert.AreEqual(
            @"E:\",
            new LauncherSettings { GameDirectory = @"E:\" }.Normalize().GameDirectory);
    }

    [TestMethod]
    public void CommandBuilder_UsesOnlyAdjacentSupportFilesAndChosenSettings()
    {
        var appDirectory = Path.Combine(Path.GetTempPath(), "World War VR");
        var gameDirectory = Path.Combine(Path.GetTempPath(), "Call of Duty World at War");
        var settings = new LauncherSettings
        {
            GameDirectory = gameDirectory,
            LaunchTarget = GameLaunchTarget.Multiplayer,
            AutomaticBots = false,
            Resolution = VrResolutionPreset.HighQuality,
        };

        var command = LauncherCommandBuilder.Build(appDirectory, settings);

        Assert.AreEqual(
            Path.Combine(Path.GetFullPath(appDirectory), "wawvr-launcher.exe"),
            command.FileName);
        CollectionAssert.AreEqual(
            new[]
            {
                "--launch", "--multiplayer", "--game-dir",
                Path.GetFullPath(gameDirectory), "--resolution", "3200x1800",
                "--mod-dll", Path.Combine(Path.GetFullPath(appDirectory), "WorldWarVR.dll"),
                "--bots", "disabled",
            },
            command.Arguments.ToArray());
    }

    [TestMethod]
    public void CommandBuilder_UsesMenuAndRecommendedDefaults()
    {
        var settings = new LauncherSettings { GameDirectory = @"C:\Games\WaW" };
        var command = LauncherCommandBuilder.Build(@"C:\WorldWarVR", settings);

        CollectionAssert.Contains(command.Arguments.ToArray(), "--menu");
        CollectionAssert.Contains(command.Arguments.ToArray(), "2560x1440");
        CollectionAssert.Contains(command.Arguments.ToArray(), "enabled");
    }

    [TestMethod]
    public void CommandBuilder_PinsValidatedSplitInstallationExecutables()
    {
        var gameDirectory = Path.Combine(Path.GetTempPath(), "WaW game data");
        var spExecutable = Path.Combine(Path.GetTempPath(), "verified", "t4sp.exe");
        var mpExecutable = Path.Combine(Path.GetTempPath(), "verified", "t4mp.exe");
        var settings = new LauncherSettings
        {
            GameDirectory = gameDirectory,
            LaunchTarget = GameLaunchTarget.MainMenu,
        };
        var validation = new GameInstallationValidation(
            Path.GetFullPath(gameDirectory),
            true,
            true,
            true,
            SupportedGameBuild.CompatibleBuild,
            SupportedGameBuild.CompatibleBuild,
            "ready",
            spExecutable,
            mpExecutable);

        var command = LauncherCommandBuilder.Build(
            Path.GetTempPath(),
            settings,
            validation);

        CollectionAssert.AreEqual(
            new[]
            {
                "--source-exe", Path.GetFullPath(spExecutable),
                "--mp-source-exe", Path.GetFullPath(mpExecutable),
            },
            command.Arguments.TakeLast(4).ToArray());

        var multiplayerCommand = LauncherCommandBuilder.Build(
            Path.GetTempPath(),
            settings with { LaunchTarget = GameLaunchTarget.Multiplayer },
            validation);
        CollectionAssert.AreEqual(
            new[] { "--source-exe", Path.GetFullPath(mpExecutable) },
            multiplayerCommand.Arguments.TakeLast(2).ToArray());
        CollectionAssert.DoesNotContain(
            multiplayerCommand.Arguments.ToArray(),
            "--mp-source-exe");
    }

    [TestMethod]
    public async Task SettingsStore_RoundTripsEnumsAndNormalizesPath()
    {
        var root = Path.Combine(Path.GetTempPath(), $"wawvr-settings-{Guid.NewGuid():N}");
        var path = Path.Combine(root, "launcher-settings.json");
        try
        {
            var store = new LauncherSettingsStore(path);
            await store.SaveAsync(new LauncherSettings
            {
                GameDirectory = @"C:\Games\WaW\",
                LaunchTarget = GameLaunchTarget.Multiplayer,
                AutomaticBots = false,
                QuestAirLinkCompatibility = true,
                Resolution = VrResolutionPreset.Performance,
            });

            var loaded = await store.LoadAsync();
            Assert.AreEqual(@"C:\Games\WaW", loaded.GameDirectory);
            Assert.AreEqual(GameLaunchTarget.Multiplayer, loaded.LaunchTarget);
            Assert.IsFalse(loaded.AutomaticBots);
            Assert.IsTrue(loaded.QuestAirLinkCompatibility);
            Assert.AreEqual(VrResolutionPreset.Performance, loaded.Resolution);

            var json = await File.ReadAllTextAsync(path);
            StringAssert.Contains(json, "Multiplayer");
            StringAssert.Contains(json, "Performance");
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [TestMethod]
    public async Task SettingsStore_RoundTripsUpdaterStateAndMigratesVersionOne()
    {
        var root = Path.Combine(Path.GetTempPath(), $"wawvr-settings-migration-{Guid.NewGuid():N}");
        var path = Path.Combine(root, "launcher-settings.json");
        try
        {
            Directory.CreateDirectory(root);
            await File.WriteAllTextAsync(path, """
                {
                  "Version": 1,
                  "GameDirectory": "C:\\\\Games\\\\WaW\\\\",
                  "LaunchTarget": "MainMenu",
                  "AutomaticBots": true,
                  "Resolution": "Recommended"
                }
                """);
            var store = new LauncherSettingsStore(path);
            var migrated = await store.LoadAsync();

            Assert.AreEqual(3, migrated.Version);
            Assert.AreEqual(@"C:\Games\WaW", migrated.GameDirectory);
            Assert.IsFalse(migrated.QuestAirLinkCompatibility);
            Assert.IsNull(migrated.LastUpdateCheckUtc);
            Assert.IsNull(migrated.UpdatePromptSnoozedUntilUtc);
            Assert.IsNull(migrated.CachedUpdate);

            var checkedAt = new DateTimeOffset(2026, 8, 17, 12, 0, 0, TimeSpan.Zero);
            var snoozedUntil = checkedAt + GitHubUpdateService.PromptSnoozeDuration;
            var release = new UpdateReleaseSummary(
                "v0.5.0-alpha.2",
                SemanticVersion.Parse("0.5.0-alpha.2"),
                "Alpha 2",
                "Release notes",
                new Uri("https://github.com/RyanCraighead/WorldWarVR-Releases/releases/tag/v0.5.0-alpha.2"));
            await store.SaveAsync(migrated with
            {
                LastUpdateCheckUtc = checkedAt,
                UpdatePromptSnoozedUntilUtc = snoozedUntil,
                CachedUpdate = CachedUpdateInfo.FromRelease(release),
            });

            var loaded = await store.LoadAsync();
            Assert.AreEqual(checkedAt, loaded.LastUpdateCheckUtc);
            Assert.AreEqual(snoozedUntil, loaded.UpdatePromptSnoozedUntilUtc);
            Assert.IsNotNull(loaded.CachedUpdate);
            Assert.IsTrue(loaded.CachedUpdate.TryGetRelease(out var cached));
            Assert.AreEqual(release.Version, cached!.Version);
            Assert.AreEqual(release.ReleaseNotes, cached.ReleaseNotes);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [TestMethod]
    public void SteamVrAirLinkCompatibility_SelectsRegisteredX86RuntimeAndCurrentMetaRoot()
    {
        var root = Path.Combine(Path.GetTempPath(), $"wawvr-air-link-{Guid.NewGuid():N}");
        var steamVrDirectory = Path.Combine(root, "moved-steam", "SteamVR");
        var metaDirectory = Path.Combine(root, "moved-meta", "Meta Horizon");
        var manifest = Path.Combine(steamVrDirectory, "steamxr_win32.json");
        try
        {
            Directory.CreateDirectory(steamVrDirectory);
            Directory.CreateDirectory(metaDirectory);
            File.WriteAllText(manifest, "{}");

            Assert.AreEqual(
                Path.GetFullPath(manifest),
                SteamVrAirLinkCompatibility.SelectSteamVrX86Manifest(
                    new[]
                    {
                        Path.Combine(root, "steamxr_win64.json"),
                        manifest,
                    }));
            Assert.AreEqual(
                Path.GetFullPath(metaDirectory),
                SteamVrAirLinkCompatibility.SelectCurrentOculusBase(
                    metaDirectory,
                    Path.Combine(root, "stale-meta")));
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [TestMethod]
    public async Task SettingsStore_ReturnsDefaultsForMalformedJson()
    {
        var root = Path.Combine(Path.GetTempPath(), $"wawvr-settings-{Guid.NewGuid():N}");
        var path = Path.Combine(root, "launcher-settings.json");
        try
        {
            Directory.CreateDirectory(root);
            await File.WriteAllTextAsync(path, "{ not json");
            var loaded = await new LauncherSettingsStore(path).LoadAsync();
            Assert.AreEqual(new LauncherSettings(), loaded);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [TestMethod]
    public async Task Validator_RejectsFolderWithoutGameLayoutBeforeHashing()
    {
        var root = Path.Combine(Path.GetTempPath(), $"wawvr-invalid-{Guid.NewGuid():N}");
        try
        {
            Directory.CreateDirectory(root);
            var result = await GameInstallationValidator.ValidateAsync(root);
            Assert.IsFalse(result.MainMenuReady);
            Assert.IsFalse(result.MultiplayerReady);
            StringAssert.Contains(result.Detail, "main, zone");
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [TestMethod]
    public void Validator_UsesOnlyExactLegacyCompatibleFallbackLocations()
    {
        var localApplicationData = Path.Combine(Path.GetTempPath(), "LocalAppData");

        CollectionAssert.AreEqual(
            new[]
            {
                Path.Combine(localApplicationData, "Plutonium", "games", "t4sp.exe"),
                Path.Combine(
                    localApplicationData,
                    "WaWVR",
                    "runtime",
                    "waw-1.7.1263",
                    "CoDWaW.exe"),
            },
            GameInstallationValidator.GetManagedFallbackExecutablePaths(
                localApplicationData,
                GameLaunchTarget.MainMenu).ToArray());
        CollectionAssert.AreEqual(
            new[]
            {
                Path.Combine(localApplicationData, "Plutonium", "games", "t4mp.exe"),
                Path.Combine(
                    localApplicationData,
                    "WaWVR",
                    "runtime",
                    "waw-mp-1.7.1263",
                    "CoDWaWmp.exe"),
            },
            GameInstallationValidator.GetManagedFallbackExecutablePaths(
                localApplicationData,
                GameLaunchTarget.Multiplayer).ToArray());
    }

    [TestMethod]
    public async Task Validator_RestoresSplitInstallWithoutWeakeningDirectExecutableChecks()
    {
        var root = Path.Combine(Path.GetTempPath(), $"wawvr-split-{Guid.NewGuid():N}");
        var gameDirectory = Path.Combine(root, "game-data");
        var localApplicationData = Path.Combine(root, "local-app-data");
        try
        {
            Directory.CreateDirectory(Path.Combine(gameDirectory, "main"));
            Directory.CreateDirectory(Path.Combine(gameDirectory, "zone"));

            var directSp = Path.Combine(gameDirectory, "CoDWaW.exe");
            var directMp = Path.Combine(gameDirectory, "CoDWaWmp.exe");
            var fallbackSp = GameInstallationValidator
                .GetManagedFallbackExecutablePaths(
                    localApplicationData,
                    GameLaunchTarget.MainMenu)[0];
            var fallbackMp = GameInstallationValidator
                .GetManagedFallbackExecutablePaths(
                    localApplicationData,
                    GameLaunchTarget.Multiplayer)[0];
            var hashes = new Dictionary<string, string>(
                StringComparer.OrdinalIgnoreCase)
            {
                [Path.Combine(gameDirectory, "binkw32.dll")] =
                    GameInstallationValidator.SupportedBinkSha256,
                [fallbackSp] = GameInstallationValidator.CompatibleSinglePlayerSha256,
                [fallbackMp] = GameInstallationValidator.CompatibleMultiplayerSha256,
            };

            Task<string?> HashFile(string path, CancellationToken cancellationToken)
            {
                cancellationToken.ThrowIfCancellationRequested();
                return Task.FromResult(
                    hashes.TryGetValue(Path.GetFullPath(path), out var hash)
                        ? hash
                        : null);
            }

            var split = await GameInstallationValidator.ValidateAsync(
                gameDirectory,
                CancellationToken.None,
                localApplicationData,
                HashFile);
            Assert.IsTrue(split.MainMenuReady);
            Assert.IsTrue(split.MultiplayerReady);
            Assert.AreEqual(Path.GetFullPath(fallbackSp), split.SinglePlayerExecutable);
            Assert.AreEqual(Path.GetFullPath(fallbackMp), split.MultiplayerExecutable);

            hashes[directSp] = GameInstallationValidator.SteamSinglePlayerSha256;
            hashes[directMp] = GameInstallationValidator.SteamMultiplayerSha256;
            var completeSteam = await GameInstallationValidator.ValidateAsync(
                gameDirectory,
                CancellationToken.None,
                localApplicationData,
                HashFile);
            Assert.AreEqual(
                SupportedGameBuild.SteamBuild252004,
                completeSteam.SinglePlayerBuild);
            Assert.AreEqual(
                SupportedGameBuild.SteamBuild252004,
                completeSteam.MultiplayerBuild);
            Assert.AreEqual(Path.GetFullPath(directSp), completeSteam.SinglePlayerExecutable);
            Assert.AreEqual(Path.GetFullPath(directMp), completeSteam.MultiplayerExecutable);

            hashes[directSp] = "UNSUPPORTED-SP";
            hashes[directMp] = "UNSUPPORTED-MP";
            var unsupportedDirect = await GameInstallationValidator.ValidateAsync(
                gameDirectory,
                CancellationToken.None,
                localApplicationData,
                HashFile);
            Assert.IsFalse(unsupportedDirect.MainMenuReady);
            Assert.IsFalse(unsupportedDirect.MultiplayerReady);
            Assert.AreEqual(Path.GetFullPath(directSp), unsupportedDirect.SinglePlayerExecutable);
            Assert.AreEqual(Path.GetFullPath(directMp), unsupportedDirect.MultiplayerExecutable);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [TestMethod]
    public void ResolutionChoices_HaveStableProductPresets()
    {
        CollectionAssert.AreEqual(
            new[] { "2560x1440", "3200x1800", "1920x1080" },
            ResolutionChoices.All.Select(choice => choice.Dimensions).ToArray());
    }

    [TestMethod]
    public void CandidateSelection_SkipsStalePathsAndPrefersCompleteInstallation()
    {
        var stale = new GameInstallationValidation(
            @"C:\Stale", false, false, false,
            SupportedGameBuild.None, SupportedGameBuild.None, "missing");
        var mainOnly = new GameInstallationValidation(
            @"D:\MainOnly", true, true, true,
            SupportedGameBuild.SteamBuild252004, SupportedGameBuild.None, "main only");
        var complete = new GameInstallationValidation(
            @"E:\Steam\steamapps\common\Call of Duty World at War",
            true, true, true,
            SupportedGameBuild.SteamBuild252004,
            SupportedGameBuild.SteamBuild252004,
            "complete");

        var selected = GameInstallationValidator.ChooseBestCandidate(
            [stale, mainOnly, complete]);

        Assert.AreSame(complete, selected);
    }
}
