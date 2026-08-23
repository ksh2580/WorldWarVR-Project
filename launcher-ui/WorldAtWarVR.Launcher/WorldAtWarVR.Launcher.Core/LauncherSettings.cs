using System.Text.Json;
using System.Text.Json.Serialization;

namespace WorldAtWarVR.Launcher.Core;

public enum GameLaunchTarget
{
    MainMenu,
    Multiplayer,
}

public enum VrResolutionPreset
{
    Recommended,
    HighQuality,
    Performance,
}

public sealed record ResolutionChoice(
    VrResolutionPreset Preset,
    string Name,
    string Detail,
    int Width,
    int Height)
{
    public string Dimensions => $"{Width}x{Height}";
}

public static class ResolutionChoices
{
    public static IReadOnlyList<ResolutionChoice> All { get; } =
    [
        new(VrResolutionPreset.Recommended, "Recommended", "Best balance for most headsets", 2560, 1440),
        new(VrResolutionPreset.HighQuality, "High quality", "Sharper image with a higher GPU cost", 3200, 1800),
        new(VrResolutionPreset.Performance, "Performance", "Lighter GPU load for smoother play", 1920, 1080),
    ];

    public static ResolutionChoice Get(VrResolutionPreset preset) =>
        All.FirstOrDefault(choice => choice.Preset == preset) ?? All[0];
}

public sealed record LauncherSettings
{
    public int Version { get; init; } = 3;
    public string GameDirectory { get; init; } = string.Empty;
    public GameLaunchTarget LaunchTarget { get; init; } = GameLaunchTarget.MainMenu;
    public bool AutomaticBots { get; init; } = true;
    public bool QuestAirLinkCompatibility { get; init; }
    public VrResolutionPreset Resolution { get; init; } = VrResolutionPreset.Recommended;
    public DateTimeOffset? LastUpdateCheckUtc { get; init; }
    public DateTimeOffset? UpdatePromptSnoozedUntilUtc { get; init; }
    public CachedUpdateInfo? CachedUpdate { get; init; }

    public LauncherSettings Normalize()
    {
        var target = Enum.IsDefined(LaunchTarget) ? LaunchTarget : GameLaunchTarget.MainMenu;
        var resolution = Enum.IsDefined(Resolution) ? Resolution : VrResolutionPreset.Recommended;
        var directory = GameInstallationValidator.NormalizeDirectory(GameDirectory);

        return this with
        {
            Version = 3,
            GameDirectory = directory,
            LaunchTarget = target,
            Resolution = resolution,
            LastUpdateCheckUtc = NormalizeUtc(LastUpdateCheckUtc),
            UpdatePromptSnoozedUntilUtc = NormalizeUtc(UpdatePromptSnoozedUntilUtc),
            CachedUpdate = CachedUpdate?.Normalize(),
        };
    }

    private static DateTimeOffset? NormalizeUtc(DateTimeOffset? value) =>
        value?.ToUniversalTime();
}

public sealed record CachedUpdateInfo
{
    public string TagName { get; init; } = string.Empty;
    public string Version { get; init; } = string.Empty;
    public string Title { get; init; } = string.Empty;
    public string ReleaseNotes { get; init; } = string.Empty;
    public string ReleasePageUrl { get; init; } = string.Empty;

    public static CachedUpdateInfo FromRelease(UpdateReleaseSummary release) => new()
    {
        TagName = release.TagName,
        Version = release.Version.ToString(),
        Title = release.Title,
        ReleaseNotes = release.ReleaseNotes,
        ReleasePageUrl = release.ReleasePageUri.AbsoluteUri,
    };

    public bool TryGetRelease(out UpdateReleaseSummary? release)
    {
        release = null;
        if (!SemanticVersion.TryParse(Version, out var version) ||
            !SemanticVersion.TryParse(TagName, out var tagVersion) ||
            version!.CompareTo(tagVersion) != 0 ||
            !Uri.TryCreate(ReleasePageUrl, UriKind.Absolute, out var releaseUri) ||
            !GitHubUpdateService.IsExpectedReleasePageUri(releaseUri))
        {
            return false;
        }

        release = new UpdateReleaseSummary(
            TagName,
            version,
            GitHubUpdateService.SanitizeReleaseNotes(Title).Replace('\n', ' '),
            GitHubUpdateService.SanitizeReleaseNotes(ReleaseNotes),
            releaseUri);
        return true;
    }

    internal CachedUpdateInfo? Normalize() => TryGetRelease(out var release)
        ? FromRelease(release!)
        : null;
}

public sealed class LauncherSettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
    };
    private readonly SemaphoreSlim _saveGate = new(1, 1);

    public LauncherSettingsStore(string? settingsPath = null)
    {
        SettingsPath = settingsPath ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "WorldAtWarVR",
            "launcher-settings.json");
    }

    public string SettingsPath { get; }

    public async Task<LauncherSettings> LoadAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            if (!File.Exists(SettingsPath))
            {
                return new LauncherSettings();
            }

            await using var stream = File.OpenRead(SettingsPath);
            var settings = await JsonSerializer.DeserializeAsync<LauncherSettings>(
                stream, JsonOptions, cancellationToken);
            return (settings ?? new LauncherSettings()).Normalize();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            return new LauncherSettings();
        }
    }

    public async Task SaveAsync(
        LauncherSettings settings,
        CancellationToken cancellationToken = default)
    {
        await _saveGate.WaitAsync(cancellationToken);
        try
        {
            var normalized = settings.Normalize();
            var directory = Path.GetDirectoryName(SettingsPath)
                ?? throw new InvalidOperationException("The settings path has no parent directory.");
            Directory.CreateDirectory(directory);

            var temporaryPath = SettingsPath + ".tmp";
            try
            {
                await using (var stream = new FileStream(
                    temporaryPath,
                    FileMode.Create,
                    FileAccess.Write,
                    FileShare.None,
                    4096,
                    useAsync: true))
                {
                    await JsonSerializer.SerializeAsync(
                        stream, normalized, JsonOptions, cancellationToken);
                    await stream.FlushAsync(cancellationToken);
                }

                File.Move(temporaryPath, SettingsPath, overwrite: true);
            }
            finally
            {
                if (File.Exists(temporaryPath))
                {
                    File.Delete(temporaryPath);
                }
            }
        }
        finally
        {
            _saveGate.Release();
        }
    }
}
