using System.Globalization;
using System.Reflection;

namespace WorldAtWarVR.Launcher.Core;

/// <summary>
/// A strict SemVer 2.0 value. Build metadata is retained for display but does
/// not participate in precedence comparisons.
/// </summary>
public sealed class SemanticVersion : IComparable<SemanticVersion>, IEquatable<SemanticVersion>
{
    private SemanticVersion(
        int major,
        int minor,
        int patch,
        IReadOnlyList<string> prerelease,
        string? buildMetadata)
    {
        Major = major;
        Minor = minor;
        Patch = patch;
        Prerelease = prerelease;
        BuildMetadata = buildMetadata;
    }

    public int Major { get; }
    public int Minor { get; }
    public int Patch { get; }
    public IReadOnlyList<string> Prerelease { get; }
    public string? BuildMetadata { get; }

    public static bool TryParse(string? value, out SemanticVersion? version)
    {
        version = null;
        if (string.IsNullOrWhiteSpace(value))
        {
            return false;
        }

        var text = value.Trim();
        if (text.StartsWith('v') || text.StartsWith('V'))
        {
            text = text[1..];
        }

        var plus = text.IndexOf('+');
        var build = plus >= 0 ? text[(plus + 1)..] : null;
        var withoutBuild = plus >= 0 ? text[..plus] : text;
        if (plus >= 0 && (!ValidIdentifierList(build!, allowLeadingZeroNumbers: true) || text.IndexOf('+', plus + 1) >= 0))
        {
            return false;
        }

        var dash = withoutBuild.IndexOf('-');
        var prereleaseText = dash >= 0 ? withoutBuild[(dash + 1)..] : null;
        var core = dash >= 0 ? withoutBuild[..dash] : withoutBuild;
        if (dash >= 0 && !ValidIdentifierList(prereleaseText!, allowLeadingZeroNumbers: false))
        {
            return false;
        }

        var coreParts = core.Split('.');
        if (coreParts.Length != 3 ||
            !TryParseCoreNumber(coreParts[0], out var major) ||
            !TryParseCoreNumber(coreParts[1], out var minor) ||
            !TryParseCoreNumber(coreParts[2], out var patch))
        {
            return false;
        }

        version = new SemanticVersion(
            major,
            minor,
            patch,
            prereleaseText?.Split('.') ?? [],
            build);
        return true;
    }

    public static SemanticVersion Parse(string value) =>
        TryParse(value, out var version)
            ? version!
            : throw new FormatException($"'{value}' is not a valid semantic version.");

    public int CompareTo(SemanticVersion? other)
    {
        if (other is null)
        {
            return 1;
        }

        var comparison = Major.CompareTo(other.Major);
        if (comparison == 0) comparison = Minor.CompareTo(other.Minor);
        if (comparison == 0) comparison = Patch.CompareTo(other.Patch);
        if (comparison != 0) return comparison;

        if (Prerelease.Count == 0 || other.Prerelease.Count == 0)
        {
            return Prerelease.Count == other.Prerelease.Count
                ? 0
                : Prerelease.Count == 0 ? 1 : -1;
        }

        var shared = Math.Min(Prerelease.Count, other.Prerelease.Count);
        for (var index = 0; index < shared; index++)
        {
            comparison = ComparePrereleaseIdentifier(Prerelease[index], other.Prerelease[index]);
            if (comparison != 0)
            {
                return comparison;
            }
        }

        return Prerelease.Count.CompareTo(other.Prerelease.Count);
    }

    public bool Equals(SemanticVersion? other) => CompareTo(other) == 0;
    public override bool Equals(object? obj) => obj is SemanticVersion other && Equals(other);
    public override int GetHashCode()
    {
        var hash = new HashCode();
        hash.Add(Major);
        hash.Add(Minor);
        hash.Add(Patch);
        foreach (var identifier in Prerelease)
        {
            hash.Add(identifier, StringComparer.Ordinal);
        }
        return hash.ToHashCode();
    }

    public override string ToString()
    {
        var value = $"{Major}.{Minor}.{Patch}";
        if (Prerelease.Count > 0) value += $"-{string.Join('.', Prerelease)}";
        if (!string.IsNullOrEmpty(BuildMetadata)) value += $"+{BuildMetadata}";
        return value;
    }

    public static bool operator >(SemanticVersion left, SemanticVersion right) => left.CompareTo(right) > 0;
    public static bool operator <(SemanticVersion left, SemanticVersion right) => left.CompareTo(right) < 0;
    public static bool operator >=(SemanticVersion left, SemanticVersion right) => left.CompareTo(right) >= 0;
    public static bool operator <=(SemanticVersion left, SemanticVersion right) => left.CompareTo(right) <= 0;

    private static bool TryParseCoreNumber(string text, out int value)
    {
        value = 0;
        return text.Length > 0 &&
            (text.Length == 1 || text[0] != '0') &&
            text.All(char.IsAsciiDigit) &&
            int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);
    }

    private static bool ValidIdentifierList(string value, bool allowLeadingZeroNumbers)
    {
        if (string.IsNullOrEmpty(value))
        {
            return false;
        }

        foreach (var identifier in value.Split('.'))
        {
            if (identifier.Length == 0 ||
                identifier.Any(character => !char.IsAsciiLetterOrDigit(character) && character != '-'))
            {
                return false;
            }

            var numeric = identifier.All(char.IsAsciiDigit);
            if (numeric && !allowLeadingZeroNumbers && identifier.Length > 1 && identifier[0] == '0')
            {
                return false;
            }
        }

        return true;
    }

    private static int ComparePrereleaseIdentifier(string left, string right)
    {
        var leftNumeric = left.All(char.IsAsciiDigit);
        var rightNumeric = right.All(char.IsAsciiDigit);
        if (leftNumeric && rightNumeric)
        {
            var lengthComparison = left.Length.CompareTo(right.Length);
            return lengthComparison != 0
                ? lengthComparison
                : string.CompareOrdinal(left, right);
        }

        if (leftNumeric != rightNumeric)
        {
            return leftNumeric ? -1 : 1;
        }

        return string.CompareOrdinal(left, right);
    }
}

public static class InstalledVersionProvider
{
    public static SemanticVersion Get(Assembly assembly)
    {
        var informational = assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion;
        if (SemanticVersion.TryParse(informational, out var semanticVersion))
        {
            return semanticVersion!;
        }

        var version = assembly.GetName().Version;
        return SemanticVersion.Parse(version is null
            ? "0.0.0"
            : $"{Math.Max(0, version.Major)}.{Math.Max(0, version.Minor)}.{Math.Max(0, version.Build)}");
    }
}
