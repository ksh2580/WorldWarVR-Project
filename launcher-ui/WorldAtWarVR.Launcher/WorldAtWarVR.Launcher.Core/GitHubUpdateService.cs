using System.Buffers;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace WorldAtWarVR.Launcher.Core;

public sealed record UpdateReleaseSummary(
    string TagName,
    SemanticVersion Version,
    string Title,
    string ReleaseNotes,
    Uri ReleasePageUri);

public sealed record UpdateAsset(Uri DownloadUri, long Size, string Sha256);

public sealed record AvailableUpdate(UpdateReleaseSummary Release, UpdateAsset Asset);

public sealed record UpdateDownloadProgress(long BytesReceived, long TotalBytes)
{
    public int Percentage => TotalBytes <= 0
        ? 0
        : (int)Math.Clamp(BytesReceived * 100L / TotalBytes, 0, 100);
}

public sealed class GitHubUpdateService
{
    public const string Owner = "RyanCraighead";
    public const string Repository = "WorldWarVR-Releases";
    public const string InstallerFileName = "WorldWarVR-Setup.exe";
    public static readonly TimeSpan CheckInterval = TimeSpan.FromDays(1);
    public static readonly TimeSpan CheckTimeout = TimeSpan.FromSeconds(15);
    public static readonly TimeSpan PromptSnoozeDuration = TimeSpan.FromDays(30);

    private static readonly Uri ReleasesApiUri = new(
        $"https://api.github.com/repos/{Owner}/{Repository}/releases?per_page=100");
    private static readonly HttpClient SharedHttpClient = CreateHttpClient();

    private readonly HttpClient _httpClient;
    private readonly Uri _releasesApiUri;
    private readonly string _temporaryRoot;

    public GitHubUpdateService()
        : this(SharedHttpClient, ReleasesApiUri, Path.GetTempPath())
    {
    }

    public GitHubUpdateService(
        HttpClient httpClient,
        Uri releasesApiUri,
        string? temporaryRoot = null)
    {
        _httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
        _releasesApiUri = releasesApiUri ?? throw new ArgumentNullException(nameof(releasesApiUri));
        _temporaryRoot = Path.GetFullPath(temporaryRoot ?? Path.GetTempPath());
    }

    public async Task<AvailableUpdate?> FindAvailableUpdateAsync(
        SemanticVersion installedVersion,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(installedVersion);

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(CheckTimeout);
        var operationToken = timeout.Token;

        using var request = new HttpRequestMessage(HttpMethod.Get, _releasesApiUri);
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        request.Headers.UserAgent.ParseAdd("WorldWarVR-Launcher/1.0");
        request.Headers.Add("X-GitHub-Api-Version", "2022-11-28");

        using var response = await _httpClient.SendAsync(
            request,
            HttpCompletionOption.ResponseHeadersRead,
            operationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();

        await using var stream = await response.Content.ReadAsStreamAsync(operationToken).ConfigureAwait(false);
        using var document = await JsonDocument.ParseAsync(stream, cancellationToken: operationToken).ConfigureAwait(false);
        if (document.RootElement.ValueKind != JsonValueKind.Array)
        {
            throw new InvalidDataException("GitHub returned an invalid releases response.");
        }

        AvailableUpdate? newest = null;
        foreach (var releaseElement in document.RootElement.EnumerateArray())
        {
            var candidate = ParseRelease(releaseElement);
            if (candidate is null || candidate.Release.Version <= installedVersion)
            {
                continue;
            }

            if (newest is null || candidate.Release.Version > newest.Release.Version)
            {
                newest = candidate;
            }
        }

        return newest;
    }

    public async Task<string> DownloadInstallerAsync(
        AvailableUpdate update,
        IProgress<UpdateDownloadProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(update);
        ValidateAsset(update.Asset);

        var downloadRoot = Path.Combine(
            _temporaryRoot,
            $"WorldWarVR-Update-{Guid.NewGuid():N}");
        Directory.CreateDirectory(downloadRoot);
        var partialPath = Path.Combine(downloadRoot, InstallerFileName + ".download");
        var finalPath = Path.Combine(downloadRoot, InstallerFileName);

        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, update.Asset.DownloadUri);
            request.Headers.UserAgent.ParseAdd("WorldWarVR-Launcher/1.0");
            using var response = await _httpClient.SendAsync(
                request,
                HttpCompletionOption.ResponseHeadersRead,
                cancellationToken).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();

            ValidateDownloadResponse(response, update.Asset);

            await using var input = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
            await using var output = new FileStream(
                partialPath,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                128 * 1024,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var buffer = ArrayPool<byte>.Shared.Rent(128 * 1024);
            long received = 0;
            try
            {
                while (true)
                {
                    var count = await input.ReadAsync(buffer.AsMemory(0, buffer.Length), cancellationToken).ConfigureAwait(false);
                    if (count == 0)
                    {
                        break;
                    }

                    received += count;
                    if (received > update.Asset.Size)
                    {
                        throw new InvalidDataException("The update download exceeded its published size.");
                    }

                    hash.AppendData(buffer, 0, count);
                    await output.WriteAsync(buffer.AsMemory(0, count), cancellationToken).ConfigureAwait(false);
                    progress?.Report(new UpdateDownloadProgress(received, update.Asset.Size));
                }
            }
            finally
            {
                ArrayPool<byte>.Shared.Return(buffer);
            }

            await output.FlushAsync(cancellationToken).ConfigureAwait(false);
            await output.DisposeAsync().ConfigureAwait(false);
            if (received != update.Asset.Size)
            {
                throw new InvalidDataException("The update download size did not match the GitHub release.");
            }

            var actualDigest = Convert.ToHexString(hash.GetHashAndReset());
            if (!actualDigest.Equals(update.Asset.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException("The update download failed SHA-256 verification.");
            }

            File.Move(partialPath, finalPath);
            return finalPath;
        }
        catch
        {
            try
            {
                if (Directory.Exists(downloadRoot))
                {
                    Directory.Delete(downloadRoot, recursive: true);
                }
            }
            catch
            {
                // Preserve the original update failure.
            }

            throw;
        }
    }

    public async Task VerifyInstallerAsync(
        string installerPath,
        UpdateAsset asset,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(installerPath);
        ArgumentNullException.ThrowIfNull(asset);
        ValidateAsset(asset);

        var fullPath = Path.GetFullPath(installerPath);
        if (!Path.GetFileName(fullPath).Equals(InstallerFileName, StringComparison.Ordinal))
        {
            throw new InvalidDataException("The update installer filename is invalid.");
        }

        await using var input = new FileStream(
            fullPath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            128 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (input.Length != asset.Size)
        {
            throw new InvalidDataException("The update installer size changed after download.");
        }

        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = ArrayPool<byte>.Shared.Rent(128 * 1024);
        long received = 0;
        try
        {
            while (true)
            {
                var count = await input.ReadAsync(
                    buffer.AsMemory(0, buffer.Length),
                    cancellationToken).ConfigureAwait(false);
                if (count == 0)
                {
                    break;
                }

                received += count;
                if (received > asset.Size)
                {
                    throw new InvalidDataException("The update installer size changed after download.");
                }

                hash.AppendData(buffer, 0, count);
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }

        var digest = Convert.ToHexString(hash.GetHashAndReset());
        if (received != asset.Size ||
            !digest.Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("The update installer failed its final SHA-256 verification.");
        }
    }

    public static bool ShouldCheck(DateTimeOffset? lastCheckUtc, DateTimeOffset nowUtc)
    {
        if (!lastCheckUtc.HasValue)
        {
            return true;
        }

        var elapsed = nowUtc - lastCheckUtc.Value;
        return elapsed < TimeSpan.Zero || elapsed >= CheckInterval;
    }

    public static bool IsPromptSnoozed(DateTimeOffset? snoozedUntilUtc, DateTimeOffset nowUtc) =>
        snoozedUntilUtc.HasValue && snoozedUntilUtc.Value > nowUtc;

    public static string SanitizeReleaseNotes(string? notes)
    {
        if (string.IsNullOrWhiteSpace(notes))
        {
            return "No release notes were provided.";
        }

        const int maximumLength = 64 * 1024;
        var builder = new StringBuilder(Math.Min(notes.Length, maximumLength));
        foreach (var character in notes.Replace("\r\n", "\n", StringComparison.Ordinal).Replace('\r', '\n'))
        {
            if (character == '\n' || character == '\t' || !char.IsControl(character))
            {
                builder.Append(character);
            }

            if (builder.Length == maximumLength)
            {
                builder.Append("\n\n[Release notes truncated by the launcher.]");
                break;
            }
        }

        return builder.ToString().Trim();
    }

    private static AvailableUpdate? ParseRelease(JsonElement element)
    {
        if (element.ValueKind != JsonValueKind.Object ||
            ReadBoolean(element, "draft") ||
            !TryReadString(element, "tag_name", out var tagName) ||
            !SemanticVersion.TryParse(tagName, out var version) ||
            !TryReadHttpsUri(element, "html_url", out var releasePageUri) ||
            !IsExpectedReleasePageUri(releasePageUri) ||
            !element.TryGetProperty("assets", out var assets) ||
            assets.ValueKind != JsonValueKind.Array)
        {
            return null;
        }

        JsonElement? exactAsset = null;
        foreach (var asset in assets.EnumerateArray())
        {
            if (asset.ValueKind == JsonValueKind.Object &&
                TryReadString(asset, "name", out var name) &&
                name.Equals(InstallerFileName, StringComparison.Ordinal))
            {
                if (exactAsset.HasValue)
                {
                    return null;
                }
                exactAsset = asset;
            }
        }

        if (!exactAsset.HasValue ||
            !TryReadHttpsUri(exactAsset.Value, "browser_download_url", out var downloadUri) ||
            !IsExpectedReleaseDownloadUri(downloadUri) ||
            !exactAsset.Value.TryGetProperty("size", out var sizeElement) ||
            !sizeElement.TryGetInt64(out var size) || size <= 0 ||
            !TryReadString(exactAsset.Value, "digest", out var digest) ||
            !TryParseSha256Digest(digest, out var sha256))
        {
            return null;
        }

        var sanitizedTitle = TryReadString(element, "name", out var releaseName)
            ? SanitizeSingleLine(releaseName)
            : string.Empty;
        var title = string.IsNullOrWhiteSpace(sanitizedTitle) ? tagName : sanitizedTitle;
        var notes = TryReadString(element, "body", out var body)
            ? SanitizeReleaseNotes(body)
            : SanitizeReleaseNotes(null);

        return new AvailableUpdate(
            new UpdateReleaseSummary(tagName, version!, title, notes, releasePageUri),
            new UpdateAsset(downloadUri, size, sha256));
    }

    private static void ValidateAsset(UpdateAsset asset)
    {
        if (asset.Size <= 0 ||
            !IsExpectedReleaseDownloadUri(asset.DownloadUri) ||
            asset.Sha256.Length != 64 ||
            asset.Sha256.Any(character => !Uri.IsHexDigit(character)))
        {
            throw new InvalidDataException("The GitHub release contains invalid installer metadata.");
        }
    }

    private static void ValidateDownloadResponse(HttpResponseMessage response, UpdateAsset asset)
    {
        var finalUri = response.RequestMessage?.RequestUri
            ?? throw new InvalidDataException("The update response did not identify its source.");
        if (!IsAllowedDownloadResponseUri(finalUri))
        {
            throw new InvalidDataException("The update download was redirected outside GitHub.");
        }

        if (response.Content.Headers.ContentLength is long contentLength && contentLength != asset.Size)
        {
            throw new InvalidDataException("The update response size did not match the GitHub release.");
        }
    }

    private static bool IsExpectedReleaseDownloadUri(Uri uri)
    {
        if (!uri.IsAbsoluteUri || uri.Scheme != Uri.UriSchemeHttps ||
            !uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var expectedPrefix = $"/{Owner}/{Repository}/releases/download/";
        return uri.AbsolutePath.StartsWith(expectedPrefix, StringComparison.OrdinalIgnoreCase) &&
            uri.AbsolutePath.EndsWith($"/{InstallerFileName}", StringComparison.Ordinal);
    }

    internal static bool IsExpectedReleasePageUri(Uri uri)
    {
        if (!uri.IsAbsoluteUri || uri.Scheme != Uri.UriSchemeHttps ||
            !uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var expectedPrefix = $"/{Owner}/{Repository}/releases/tag/";
        return uri.AbsolutePath.StartsWith(expectedPrefix, StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsAllowedDownloadResponseUri(Uri uri) =>
        uri.IsAbsoluteUri && uri.Scheme == Uri.UriSchemeHttps &&
        (uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase) ||
         uri.Host.Equals("release-assets.githubusercontent.com", StringComparison.OrdinalIgnoreCase) ||
         uri.Host.Equals("objects.githubusercontent.com", StringComparison.OrdinalIgnoreCase));

    private static bool TryParseSha256Digest(string digest, out string sha256)
    {
        const string prefix = "sha256:";
        sha256 = string.Empty;
        if (!digest.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var value = digest[prefix.Length..];
        if (value.Length != 64 || value.Any(character => !Uri.IsHexDigit(character)))
        {
            return false;
        }

        sha256 = value.ToUpperInvariant();
        return true;
    }

    private static string SanitizeSingleLine(string value) =>
        new(value.Where(character => !char.IsControl(character)).Take(256).ToArray());

    private static bool ReadBoolean(JsonElement element, string propertyName) =>
        element.TryGetProperty(propertyName, out var property) &&
        property.ValueKind == JsonValueKind.True;

    private static bool TryReadString(JsonElement element, string propertyName, out string value)
    {
        value = string.Empty;
        if (!element.TryGetProperty(propertyName, out var property) || property.ValueKind != JsonValueKind.String)
        {
            return false;
        }

        value = property.GetString() ?? string.Empty;
        return true;
    }

    private static bool TryReadHttpsUri(JsonElement element, string propertyName, out Uri uri)
    {
        uri = null!;
        if (!TryReadString(element, propertyName, out var value) ||
            !Uri.TryCreate(value, UriKind.Absolute, out var parsed) ||
            parsed.Scheme != Uri.UriSchemeHttps)
        {
            return false;
        }

        uri = parsed;
        return true;
    }

    private static HttpClient CreateHttpClient() => new(new HttpClientHandler
    {
        AllowAutoRedirect = true,
        MaxAutomaticRedirections = 5,
        AutomaticDecompression = System.Net.DecompressionMethods.All,
    })
    {
        Timeout = TimeSpan.FromMinutes(10),
    };
}
