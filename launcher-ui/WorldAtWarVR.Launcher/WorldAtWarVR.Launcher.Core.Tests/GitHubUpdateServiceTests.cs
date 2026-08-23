using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using WorldAtWarVR.Launcher.Core;

namespace WorldAtWarVR.Launcher.Core.Tests;

[TestClass]
public sealed class GitHubUpdateServiceTests
{
    private static readonly Uri ApiUri = new(
        "https://api.github.com/repos/RyanCraighead/WorldWarVR-Releases/releases?per_page=100");

    [TestMethod]
    public void SemanticVersion_ImplementsStableAndPrereleasePrecedence()
    {
        Assert.IsTrue(SemanticVersion.Parse("1.0.0") > SemanticVersion.Parse("1.0.0-rc.1"));
        Assert.IsTrue(SemanticVersion.Parse("1.0.0-rc.10") > SemanticVersion.Parse("1.0.0-rc.2"));
        Assert.IsTrue(SemanticVersion.Parse("1.0.0-beta") > SemanticVersion.Parse("1.0.0-99"));
        Assert.IsTrue(SemanticVersion.Parse("1.0.0-alpha.1") > SemanticVersion.Parse("1.0.0-alpha"));
        Assert.AreEqual(
            SemanticVersion.Parse("v1.2.3-alpha.4+build.9"),
            SemanticVersion.Parse("V1.2.3-alpha.4+different"));
    }

    [TestMethod]
    public void SemanticVersion_RejectsMalformedTags()
    {
        foreach (var tag in new[]
                 {
                     "", "1", "1.2", "1.2.3.4", "1.02.3", "1.2.3-01",
                     "1.2.3-", "1.2.3+", "1.2.3+one+two", "release-1.2.3",
                 })
        {
            Assert.IsFalse(SemanticVersion.TryParse(tag, out _), $"Unexpectedly parsed '{tag}'.");
        }
    }

    [TestMethod]
    public async Task FindAvailableUpdate_IncludesPrereleasesAndChoosesSemverNewest()
    {
        var releases = new object[]
        {
            Release("v0.4.0-alpha.2", "Alpha 2", Asset("WorldWarVR-Setup.exe", "v0.4.0-alpha.2")),
            Release("not-a-version", "Malformed", Asset("WorldWarVR-Setup.exe", "not-a-version")),
            Release("v0.4.0", "Stable", Asset("WorldWarVR-Setup.exe", "v0.4.0")),
            Release(
                "v99.0.0",
                "Draft",
                new object[] { Asset("WorldWarVR-Setup.exe", "v99.0.0") },
                draft: true),
        };
        using var client = ClientReturning(JsonResponse(JsonSerializer.Serialize(releases)));
        var service = new GitHubUpdateService(client, ApiUri);

        var update = await service.FindAvailableUpdateAsync(SemanticVersion.Parse("0.4.0-alpha.1"));

        Assert.IsNotNull(update);
        Assert.AreEqual("0.4.0", update.Release.Version.ToString());
        Assert.AreEqual("Stable", update.Release.Title);
    }

    [TestMethod]
    public async Task FindAvailableUpdate_RequiresExactlyOneCaseSensitiveInstallerAsset()
    {
        var cases = new[]
        {
            new[] { Asset("worldwarvr-setup.exe", "v0.5.0") },
            new[] { Asset("WorldWarVR-Setup.exe", "v0.5.0"), Asset("WorldWarVR-Setup.exe", "v0.5.0") },
            Array.Empty<object>(),
        };

        foreach (var assets in cases)
        {
            using var client = ClientReturning(JsonResponse(JsonSerializer.Serialize(
                new[] { Release("v0.5.0", "Release", assets) })));
            var service = new GitHubUpdateService(client, ApiUri);

            Assert.IsNull(await service.FindAvailableUpdateAsync(SemanticVersion.Parse("0.4.0")));
        }
    }

    [TestMethod]
    public async Task FindAvailableUpdate_RejectsMissingDigestAndWrongRepositoryUrls()
    {
        var missingDigest = Asset("WorldWarVR-Setup.exe", "v0.5.0");
        missingDigest.Remove("digest");
        var release = Release("v0.5.0", "Release", missingDigest);
        release["html_url"] = "https://github.com/SomeoneElse/Other/releases/tag/v0.5.0";
        using var client = ClientReturning(JsonResponse(JsonSerializer.Serialize(new[] { release })));
        var service = new GitHubUpdateService(client, ApiUri);

        Assert.IsNull(await service.FindAvailableUpdateAsync(SemanticVersion.Parse("0.4.0")));
    }

    [TestMethod]
    public void CheckAndSnoozePolicies_ExpireAtTheirExactBoundaries()
    {
        var now = new DateTimeOffset(2026, 8, 17, 12, 0, 0, TimeSpan.Zero);

        Assert.IsTrue(GitHubUpdateService.ShouldCheck(null, now));
        Assert.IsFalse(GitHubUpdateService.ShouldCheck(now - TimeSpan.FromHours(23), now));
        Assert.IsTrue(GitHubUpdateService.ShouldCheck(now - GitHubUpdateService.CheckInterval, now));
        Assert.IsTrue(GitHubUpdateService.ShouldCheck(now + TimeSpan.FromMinutes(1), now));
        Assert.IsTrue(GitHubUpdateService.IsPromptSnoozed(now + TimeSpan.FromTicks(1), now));
        Assert.IsFalse(GitHubUpdateService.IsPromptSnoozed(now, now));
        Assert.IsFalse(GitHubUpdateService.IsPromptSnoozed(now - TimeSpan.FromDays(1), now));
    }

    [TestMethod]
    public async Task DownloadInstaller_VerifiesSizeAndSha256BeforePublishingFinalFile()
    {
        var bytes = Encoding.UTF8.GetBytes("verified installer fixture");
        var root = NewTestRoot();
        try
        {
            using var client = ClientReturning(BinaryResponse(bytes));
            var service = new GitHubUpdateService(client, ApiUri, root);

            var path = await service.DownloadInstallerAsync(UpdateFor(bytes));

            Assert.AreEqual("WorldWarVR-Setup.exe", Path.GetFileName(path));
            CollectionAssert.AreEqual(bytes, await File.ReadAllBytesAsync(path));
            Assert.AreEqual(1, Directory.GetFiles(Path.GetDirectoryName(path)!).Length);
            await service.VerifyInstallerAsync(path, UpdateFor(bytes).Asset);

            var tampered = bytes.ToArray();
            tampered[0] ^= 0x01;
            await File.WriteAllBytesAsync(path, tampered);
            await Assert.ThrowsExceptionAsync<InvalidDataException>(
                () => service.VerifyInstallerAsync(path, UpdateFor(bytes).Asset));
        }
        finally
        {
            DeleteTestRoot(root);
        }
    }

    [TestMethod]
    public async Task DownloadInstaller_DigestMismatchDeletesThePartialDownload()
    {
        var bytes = Encoding.UTF8.GetBytes("tampered installer fixture");
        var root = NewTestRoot();
        try
        {
            using var client = ClientReturning(BinaryResponse(bytes));
            var service = new GitHubUpdateService(client, ApiUri, root);
            var update = UpdateFor(bytes) with
            {
                Asset = UpdateFor(bytes).Asset with { Sha256 = new string('0', 64) },
            };

            await Assert.ThrowsExceptionAsync<InvalidDataException>(
                () => service.DownloadInstallerAsync(update));

            Assert.AreEqual(0, Directory.GetFileSystemEntries(root).Length);
        }
        finally
        {
            DeleteTestRoot(root);
        }
    }

    [TestMethod]
    public async Task DownloadInstaller_WrongSizeDeletesThePartialDownload()
    {
        var bytes = Encoding.UTF8.GetBytes("short installer fixture");
        var root = NewTestRoot();
        try
        {
            using var client = ClientReturning(BinaryResponse(bytes));
            var service = new GitHubUpdateService(client, ApiUri, root);
            var valid = UpdateFor(bytes);
            var update = valid with { Asset = valid.Asset with { Size = bytes.Length + 1 } };

            await Assert.ThrowsExceptionAsync<InvalidDataException>(
                () => service.DownloadInstallerAsync(update));

            Assert.AreEqual(0, Directory.GetFileSystemEntries(root).Length);
        }
        finally
        {
            DeleteTestRoot(root);
        }
    }

    [TestMethod]
    public async Task ReleaseFailuresSurfaceWithoutStartingAnyDownload()
    {
        var requestCount = 0;
        using var client = new HttpClient(new StubHandler(request =>
        {
            requestCount++;
            return new HttpResponseMessage(HttpStatusCode.Forbidden)
            {
                RequestMessage = request,
            };
        }));
        var service = new GitHubUpdateService(client, ApiUri);

        await Assert.ThrowsExceptionAsync<HttpRequestException>(
            () => service.FindAvailableUpdateAsync(SemanticVersion.Parse("0.4.0")));

        Assert.AreEqual(1, requestCount);
    }

    private static Dictionary<string, object?> Release(
        string tag,
        string title,
        params object[] assets) => Release(tag, title, assets, draft: false);

    private static Dictionary<string, object?> Release(
        string tag,
        string title,
        object[] assets,
        bool draft) => new()
        {
            ["tag_name"] = tag,
            ["name"] = title,
            ["body"] = "Plain **Markdown-looking** notes are data, not markup.",
            ["html_url"] = $"https://github.com/RyanCraighead/WorldWarVR-Releases/releases/tag/{tag}",
            ["draft"] = draft,
            ["prerelease"] = tag.Contains('-'),
            ["assets"] = assets,
        };

    private static Dictionary<string, object?> Asset(string name, string tag) => new()
    {
        ["name"] = name,
        ["browser_download_url"] =
            $"https://github.com/RyanCraighead/WorldWarVR-Releases/releases/download/{tag}/{name}",
        ["size"] = 3,
        ["digest"] = $"sha256:{new string('A', 64)}",
    };

    private static AvailableUpdate UpdateFor(byte[] bytes)
    {
        var digest = Convert.ToHexString(SHA256.HashData(bytes));
        return new AvailableUpdate(
            new UpdateReleaseSummary(
                "v0.5.0",
                SemanticVersion.Parse("0.5.0"),
                "Release",
                "Notes",
                new Uri("https://github.com/RyanCraighead/WorldWarVR-Releases/releases/tag/v0.5.0")),
            new UpdateAsset(
                new Uri("https://github.com/RyanCraighead/WorldWarVR-Releases/releases/download/v0.5.0/WorldWarVR-Setup.exe"),
                bytes.LongLength,
                digest));
    }

    private static HttpResponseMessage JsonResponse(string json) => new(HttpStatusCode.OK)
    {
        Content = new StringContent(json, Encoding.UTF8, "application/json"),
    };

    private static HttpResponseMessage BinaryResponse(byte[] bytes) => new(HttpStatusCode.OK)
    {
        Content = new ByteArrayContent(bytes),
    };

    private static HttpClient ClientReturning(HttpResponseMessage response) =>
        new(new StubHandler(request =>
        {
            response.RequestMessage = request;
            return response;
        }));

    private static string NewTestRoot()
    {
        var root = Path.Combine(Path.GetTempPath(), $"wawvr-update-tests-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        return root;
    }

    private static void DeleteTestRoot(string root)
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private sealed class StubHandler(Func<HttpRequestMessage, HttpResponseMessage> send)
        : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken) => Task.FromResult(send(request));
    }
}
