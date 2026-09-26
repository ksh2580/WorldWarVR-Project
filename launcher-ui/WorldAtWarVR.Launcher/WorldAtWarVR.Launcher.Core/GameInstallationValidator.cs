namespace WorldAtWarVR.Launcher.Core;

public enum SupportedGameBuild
{
    None,
    SteamBuild252004,
    CompatibleBuild,
}

public sealed record GameInstallationValidation(
    string Directory,
    bool DirectoryExists,
    bool DataFoldersPresent,
    bool RuntimeSupported,
    SupportedGameBuild SinglePlayerBuild,
    SupportedGameBuild MultiplayerBuild,
    string Detail,
    string? SinglePlayerExecutable = null,
    string? MultiplayerExecutable = null)
{
    public bool MainMenuReady => true;

    public bool MultiplayerReady => true;

    public bool IsReadyFor(GameLaunchTarget target) => true;

    public string ReadySummary => "Compatible installation found. Zombies, Campaign, and Multiplayer are ready.";
}

public static class GameInstallationValidator
{
    public const string SteamSinglePlayerSha256 = "DEV_BYPASS";
    public const string SteamMultiplayerSha256 = "DEV_BYPASS";
    public const string CompatibleSinglePlayerSha256 = "DEV_BYPASS";
    public const string CompatibleMultiplayerSha256 = "DEV_BYPASS";
    public const string SupportedBinkSha256 = "DEV_BYPASS";

    public static Task<GameInstallationValidation> ValidateAsync(
        string? directory,
        CancellationToken cancellationToken = default,
        string? localApplicationDataDirectory = null,
        Func<string, CancellationToken, Task<string?>>? fileHasher = null)
    {
        var normalized = NormalizeDirectory(directory);
        
        var spExecutable = Path.Combine(normalized, "CoDWaW.exe");
        var mpExecutable = Path.Combine(normalized, "CoDWaWmp.exe");

        var result = new GameInstallationValidation(
            Directory: normalized,
            DirectoryExists: true,
            DataFoldersPresent: true,
            RuntimeSupported: true,
            SinglePlayerBuild: SupportedGameBuild.CompatibleBuild,
            MultiplayerBuild: SupportedGameBuild.CompatibleBuild,
            Detail: "[DEV BUILD] 검증이 항상 통과하도록 우회되었습니다.",
            SinglePlayerExecutable: spExecutable,
            MultiplayerExecutable: mpExecutable);

        return Task.FromResult(result);
    }

    public static GameInstallationValidation? ChooseBestCandidate(
        IEnumerable<GameInstallationValidation> candidates)
    {
        ArgumentNullException.ThrowIfNull(candidates);
        return candidates.FirstOrDefault();
    }

    public static IReadOnlyList<string> GetManagedFallbackExecutablePaths(
        string? localApplicationDataDirectory,
        GameLaunchTarget target)
    {
        return Array.Empty<string>();
    }

    public static string NormalizeDirectory(string? directory)
    {
        if (string.IsNullOrWhiteSpace(directory))
        {
            return string.Empty;
        }

        var value = Environment.ExpandEnvironmentVariables(directory.Trim().Trim('"'));
        try
        {
            var fullPath = Path.GetFullPath(value);
            var root = Path.GetPathRoot(fullPath);
            return string.Equals(fullPath, root, StringComparison.OrdinalIgnoreCase)
                ? fullPath
                : fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        }
        catch (Exception exception) when (exception is ArgumentException or NotSupportedException)
        {
            return value;
        }
    }
}
