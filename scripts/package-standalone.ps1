[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidatePattern('^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-(?:alpha|beta|rc)\.(?:0|[1-9][0-9]*))?$')]
    [string]$Version = '0.4.0-alpha.1',

    [string]$InnoCompiler = '',

    [string]$BuildRoot = '',
    [string]$OutputRoot = '',
    [string]$TempRoot = '',

    [switch]$SkipTests,
    [switch]$SkipInstaller
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$null = $Version -match '^(\d+)\.(\d+)\.(\d+)'
$fileVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
foreach ($component in @($Matches[1], $Matches[2], $Matches[3])) {
    if ([uint64]$component -gt 65535) {
        throw "Version components must fit the Windows file-version range: $Version"
    }
}

$repoRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$nativeBuildScript = Join-Path $repoRoot 'scripts\build.ps1'
$launcherProject = Join-Path $repoRoot `
    'launcher-ui\WorldAtWarVR.Launcher\WorldAtWarVR.Launcher.csproj'
$launcherTestsProject = Join-Path $repoRoot `
    'launcher-ui\WorldAtWarVR.Launcher\WorldAtWarVR.Launcher.Core.Tests\WorldAtWarVR.Launcher.Core.Tests.csproj'
$installerBuildScript = Join-Path $repoRoot 'installer\build-installer.ps1'

$resolvedBuildRoot = if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'build-standalone'))
}
else {
    [System.IO.Path]::GetFullPath($BuildRoot)
}
$resolvedOutputRoot = if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'dist\standalone'))
}
else {
    [System.IO.Path]::GetFullPath($OutputRoot)
}

$nativeBuildRoot = Join-Path $resolvedBuildRoot 'native'
$nativeBuildDir = Join-Path $nativeBuildRoot `
    ("vs-{0}" -f $Configuration.ToLowerInvariant())
$launcherPublishDir = Join-Path $resolvedBuildRoot 'launcher-ui-publish'
$payloadDir = Join-Path $resolvedOutputRoot 'WorldWarVR'
$legacyPayloadDir = Join-Path $resolvedOutputRoot 'WorldAtWarVR'
$stagingDir = Join-Path $resolvedOutputRoot ".WorldWarVR.staging-$PID"
$launcherAssetsFile = Join-Path $repoRoot `
    'launcher-ui\WorldAtWarVR.Launcher\obj\project.assets.json'

function Assert-NormalDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer) {
        throw "$Description is not a directory: $Path"
    }
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must not be a junction or symbolic link: $Path"
    }
}

function Assert-DirectChildPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Candidate,

        [Parameter(Mandatory = $true)]
        [string]$Parent
    )

    $fullCandidate = [System.IO.Path]::GetFullPath($Candidate)
    $fullParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\')
    $candidateParent = [System.IO.Path]::GetDirectoryName($fullCandidate).TrimEnd('\')
    if (-not $candidateParent.Equals(
            $fullParent,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to operate outside the fixed parent: $fullCandidate"
    }
    return $fullCandidate
}

function Remove-ValidatedDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Parent
    )

    $validated = Assert-DirectChildPath -Candidate $Path -Parent $Parent
    if (-not (Test-Path -LiteralPath $validated)) {
        return
    }
    Assert-NormalDirectory -Path $validated -Description 'Generated directory'
    Remove-Item -LiteralPath $validated -Recurse -Force
}

function Assert-NormalFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must not be a symbolic link: $Path"
    }
    return $item
}

function Copy-NuGetLicenseFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$AssetsFile,

        [Parameter(Mandatory = $true)]
        [string]$DestinationRoot
    )

    $null = Assert-NormalFile `
        -Path $AssetsFile `
        -Description 'Restored NuGet asset inventory'
    $assets = Get-Content -LiteralPath $AssetsFile -Raw | ConvertFrom-Json
    $packageRoots = @(
        $assets.packageFolders.PSObject.Properties |
            ForEach-Object Name)
    if ($packageRoots.Count -eq 0) {
        throw 'NuGet asset inventory contains no package roots.'
    }

    $copied = 0
    foreach ($libraryProperty in @(
            $assets.libraries.PSObject.Properties | Sort-Object Name)) {
        $libraryKey = $libraryProperty.Name
        $library = $libraryProperty.Value
        if ($library.type -ne 'package' -or
            [string]::IsNullOrWhiteSpace([string]$library.path)) {
            continue
        }

        $packageDirectory = $null
        foreach ($packageRoot in $packageRoots) {
            $candidate = Join-Path $packageRoot ([string]$library.path)
            if (Test-Path -LiteralPath $candidate -PathType Container) {
                $packageDirectory = $candidate
                break
            }
        }
        if ($null -eq $packageDirectory) {
            throw "Restored NuGet package is missing: $libraryKey"
        }

        $licenseFiles = @(
            Get-ChildItem -LiteralPath $packageDirectory -File -Force |
                Where-Object {
                    $_.Name -match `
                        '^(LICENSE|NOTICE|THIRD[- ]?PARTY[- ]?NOTICES?)(\..+)?$'
                } |
                Sort-Object Name)
        if ($licenseFiles.Count -eq 0) {
            continue
        }

        if ($libraryKey -match '^Microsoft\.WindowsAppSDK\.ML/' -and
            $licenseFiles.Name -notcontains 'ThirdPartyNotices.txt') {
            throw "Windows App SDK ML third-party notices are missing: $libraryKey"
        }

        $safeLibraryName = ($libraryKey -replace '[^A-Za-z0-9._-]', '_')
        $libraryDestination = Join-Path $DestinationRoot $safeLibraryName
        New-Item -ItemType Directory -Path $libraryDestination | Out-Null
        foreach ($licenseFile in $licenseFiles) {
            Copy-Item `
                -LiteralPath $licenseFile.FullName `
                -Destination (Join-Path $libraryDestination $licenseFile.Name)
            ++$copied
        }
    }

    if ($copied -eq 0) {
        throw 'No dependency license files were copied from restored packages.'
    }
}

function Copy-DotNetDownloadedPackageLicenseFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$AssetsFile,

        [Parameter(Mandatory = $true)]
        [string]$DestinationRoot
    )

    $assets = Get-Content -LiteralPath $AssetsFile -Raw | ConvertFrom-Json
    $packageRoots = @(
        $assets.packageFolders.PSObject.Properties |
            ForEach-Object Name)
    $framework = @($assets.project.frameworks.PSObject.Properties)
    if ($framework.Count -ne 1) {
        throw 'Expected exactly one launcher target framework in NuGet assets.'
    }
    $downloads = @($framework[0].Value.downloadDependencies)
    if ($downloads.Count -eq 0) {
        throw 'NuGet assets contain no downloaded runtime packages.'
    }

    $copiedCoreRuntime = $false
    foreach ($download in $downloads) {
        $versionRange = ([string]$download.version).Trim()
        $versionParts = $versionRange.Trim('[', ']').Split(',')
        if ($versionParts.Count -ne 2 -or
            -not $versionParts[0].Trim().Equals(
                $versionParts[1].Trim(),
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Downloaded package is not pinned to one exact version: $($download.name) $versionRange"
        }
        $version = $versionParts[0].Trim()
        $relativePackagePath = Join-Path `
            ([string]$download.name).ToLowerInvariant() `
            $version.ToLowerInvariant()
        $packageDirectory = $null
        foreach ($packageRoot in $packageRoots) {
            $candidate = Join-Path $packageRoot $relativePackagePath
            if (Test-Path -LiteralPath $candidate -PathType Container) {
                $packageDirectory = $candidate
                break
            }
        }
        if ($null -eq $packageDirectory) {
            throw "Downloaded package directory is missing: $($download.name) $version"
        }

        $licenseFiles = @(
            Get-ChildItem -LiteralPath $packageDirectory -File -Force |
                Where-Object {
                    $_.Name -match `
                        '^(LICENSE|NOTICE|THIRD[- ]?PARTY[- ]?NOTICES?)(\..+)?$'
                } |
                Sort-Object Name)
        if ($licenseFiles.Count -eq 0) {
            continue
        }

        $destination = Join-Path `
            $DestinationRoot `
            (("{0}_{1}" -f $download.name, $version) -replace `
                '[^A-Za-z0-9._-]', '_')
        New-Item -ItemType Directory -Path $destination | Out-Null
        foreach ($licenseFile in $licenseFiles) {
            Copy-Item `
                -LiteralPath $licenseFile.FullName `
                -Destination (Join-Path $destination $licenseFile.Name)
        }
        if ($download.name -eq 'Microsoft.NETCore.App.Runtime.win-x64') {
            $copiedCoreRuntime = $true
        }
    }
    if (-not $copiedCoreRuntime) {
        throw 'The pinned x64 .NET runtime license files were not found.'
    }
}

foreach ($required in @(
        $nativeBuildScript,
        $launcherProject,
        $launcherTestsProject)) {
    $null = Assert-NormalFile -Path $required -Description 'Required build input'
}
if (-not $SkipInstaller) {
    $null = Assert-NormalFile `
        -Path $installerBuildScript `
        -Description 'Installer build script'
}

foreach ($root in @($resolvedBuildRoot, $resolvedOutputRoot)) {
    if (Test-Path -LiteralPath $root) {
        Assert-NormalDirectory -Path $root -Description 'Generated output root'
    }
    else {
        New-Item -ItemType Directory -Path $root | Out-Null
    }
}

$nativeBuildArguments = @{
    Configuration = $Configuration
    BuildRoot = $nativeBuildRoot
    Clean = $true
}
if ($SkipTests) {
    $nativeBuildArguments.SkipTests = $true
}
if (-not [string]::IsNullOrWhiteSpace($TempRoot)) {
    $nativeBuildArguments.TempRoot = [System.IO.Path]::GetFullPath($TempRoot)
}
& $nativeBuildScript @nativeBuildArguments

if (-not $SkipTests) {
    # A stale Roslyn/MSBuild server can retain a write handle to the shared
    # managed Core obj output after an earlier launcher build.  Stop only the
    # SDK-owned build servers before the release test gate so packaging is
    # deterministic and never depends on the state of a prior IDE/build run.
    & dotnet build-server shutdown
    if ($LASTEXITCODE -ne 0) {
        throw "Could not stop stale .NET build servers (exit code $LASTEXITCODE)."
    }
    & dotnet test `
        $launcherTestsProject `
        --configuration $Configuration `
        --runtime win-x64 `
        --verbosity minimal
    if ($LASTEXITCODE -ne 0) {
        throw "Launcher UI core tests failed with exit code $LASTEXITCODE."
    }
}

Remove-ValidatedDirectory `
    -Path $launcherPublishDir `
    -Parent $resolvedBuildRoot

$publishArguments = @(
    'publish',
    $launcherProject,
    '--configuration', $Configuration,
    '--runtime', 'win-x64',
    '--self-contained', 'true',
    '--output', $launcherPublishDir,
    '-p:Platform=x64',
    "-p:Version=$Version",
    "-p:FileVersion=$fileVersion",
    "-p:AssemblyVersion=$fileVersion",
    "-p:InformationalVersion=$Version",
    '-p:PublishReadyToRun=false',
    '-p:DebugType=None',
    '-p:DebugSymbols=false'
)
& dotnet @publishArguments
if ($LASTEXITCODE -ne 0) {
    throw "Launcher UI publish failed with exit code $LASTEXITCODE."
}
Assert-NormalDirectory `
    -Path $launcherPublishDir `
    -Description 'Launcher publish directory'

$launcherExecutable = Join-Path $launcherPublishDir 'WorldWarVR.exe'
$nativeHelper = Join-Path `
    $nativeBuildDir `
    "launcher\$Configuration\wawvr-launcher.exe"
$modDll = Join-Path `
    $nativeBuildDir `
    "src\mod\$Configuration\WorldWarVR.dll"

$fixedInputs = @(
    [pscustomobject]@{ Source = $nativeHelper; Destination = 'wawvr-launcher.exe' },
    [pscustomobject]@{ Source = $modDll; Destination = 'WorldWarVR.dll' },
    [pscustomobject]@{
        Source = Join-Path $repoRoot `
            'launcher-ui\WorldAtWarVR.Launcher\Assets\WorldAtWarVR-Icon.ico'
        Destination = 'WorldWarVR.ico'
    },
    [pscustomobject]@{
        Source = Join-Path $repoRoot 'launcher\scripts\import_pezbot.ps1'
        Destination = 'WaWVR-PeZBOT-Import.ps1'
    },
    [pscustomobject]@{ Source = Join-Path $repoRoot 'LICENSE'; Destination = 'LICENSE' },
    [pscustomobject]@{
        Source = Join-Path $repoRoot 'third_party\openxr-sdk\LICENSE'
        Destination = 'licenses\OpenXR-SDK\LICENSE.txt'
    },
    [pscustomobject]@{
        Source = Join-Path $repoRoot 'third_party\openxr-sdk\src\external\jsoncpp\LICENSE'
        Destination = 'licenses\JsonCpp\LICENSE.txt'
    }
)

$null = Assert-NormalFile `
    -Path $launcherExecutable `
    -Description 'Published World War VR launcher'
foreach ($input in $fixedInputs) {
    $null = Assert-NormalFile -Path $input.Source -Description 'Payload input'
}

$null = Assert-DirectChildPath -Candidate $payloadDir -Parent $resolvedOutputRoot
$null = Assert-DirectChildPath -Candidate $stagingDir -Parent $resolvedOutputRoot
if (Test-Path -LiteralPath $stagingDir) {
    throw "Refusing to reuse package staging directory: $stagingDir"
}

$stagingCreated = $false
try {
    New-Item -ItemType Directory -Path $stagingDir | Out-Null
    $stagingCreated = $true

    foreach ($publishedItem in @(
            Get-ChildItem `
                -LiteralPath $launcherPublishDir `
                -Recurse `
                -Force)) {
        if (($publishedItem.Attributes -band
                [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            $relative = [System.IO.Path]::GetRelativePath(
                $launcherPublishDir,
                $publishedItem.FullName)
            throw "Published launcher content contains a link or junction: $relative"
        }
    }

    foreach ($entry in Get-ChildItem -LiteralPath $launcherPublishDir -Force) {
        Copy-Item `
            -LiteralPath $entry.FullName `
            -Destination $stagingDir `
            -Recurse
    }

    foreach ($input in $fixedInputs) {
        $destination = Join-Path $stagingDir $input.Destination
        $destinationParent = [System.IO.Path]::GetDirectoryName($destination)
        if (-not (Test-Path -LiteralPath $destinationParent)) {
            New-Item -ItemType Directory -Path $destinationParent | Out-Null
        }
        Assert-NormalDirectory `
            -Path $destinationParent `
            -Description 'Payload destination directory'
        Copy-Item `
            -LiteralPath $input.Source `
            -Destination $destination
    }
    Copy-NuGetLicenseFiles `
        -AssetsFile $launcherAssetsFile `
        -DestinationRoot (Join-Path $stagingDir 'licenses\nuget')
    Copy-DotNetDownloadedPackageLicenseFiles `
        -AssetsFile $launcherAssetsFile `
        -DestinationRoot (Join-Path $stagingDir 'licenses\dotnet')

    $prohibitedNames = @(
        'CoDWaW.exe',
        'CoDWaWmp.exe',
        't4sp.exe',
        't4mp.exe',
        'binkw32.dll',
        'PeZBOTWAW_005p.zip',
        'THIRD-PARTY-NOTICES.md',
        'PROVENANCE_AUDIT.md',
        'PROVENANCE-AUDIT.md',
        'components.json',
        'WorldAtWarVR.exe',
        'WorldAtWarVR.dll',
        'WorldAtWarVR.ico',
        'WorldAtWarVR.pri'
    )
    $prohibitedExtensions = @('.ff', '.iwd', '.d3dbsp', '.bik', '.pdb')
    $payloadFiles = @(Get-ChildItem -LiteralPath $stagingDir -File -Recurse -Force)
    if ($payloadFiles.Count -eq 0) {
        throw 'Standalone payload is empty.'
    }
    foreach ($file in $payloadFiles) {
        $relative = [System.IO.Path]::GetRelativePath($stagingDir, $file.FullName)
        if (($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Payload file must not be a symbolic link: $relative"
        }
        if ($prohibitedNames -icontains $file.Name -or
            $file.Name -ilike 'plutonium*' -or
            $prohibitedExtensions -icontains $file.Extension) {
            throw "Forbidden game, bot, debug, or legacy file entered payload: $relative"
        }
    }

    $releaseClaimPattern =
        '(?i)t4-rtx|GPL-?3|GNU General Public|corresponding source|publication\s+(?:is\s+)?blocked'
    foreach ($file in $payloadFiles) {
        $relative = [System.IO.Path]::GetRelativePath($stagingDir, $file.FullName)
        if ($relative.StartsWith('licenses\', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        # The project's own GPL-3.0-only license is intentionally shipped at the
        # payload root (README: "the World War VR GNU GPL version 3 license").
        # This scan exists to catch stale boilerplate copied from other projects,
        # so the root LICENSE is exempt from it.
        if ($relative -ieq 'LICENSE') {
            continue
        }
        if ($file.Name -ne 'LICENSE' -and
            @('.md', '.txt', '.json', '.ps1') -notcontains $file.Extension) {
            continue
        }
        $content = Get-Content -LiteralPath $file.FullName -Raw
        if ($content -match $releaseClaimPattern) {
            throw "First-party release text contains obsolete provenance or GPL language: $relative"
        }
    }

    foreach ($pdb in @(
            Get-ChildItem -LiteralPath $stagingDir -File -Recurse -Force |
                Where-Object Extension -ieq '.pdb')) {
        throw "Debug symbols entered standalone payload: $($pdb.FullName)"
    }

    $manifestLines = @(
        'World War VR standalone installer payload',
        'Payload layout: 1',
        'No Call of Duty game file or optional bot package is included.',
        '',
        'SHA-256:'
    )
    foreach ($file in @(
            Get-ChildItem -LiteralPath $stagingDir -File -Recurse -Force |
                Sort-Object FullName)) {
        $relative = [System.IO.Path]::GetRelativePath(
            $stagingDir,
            $file.FullName).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $manifestLines += "$hash  $relative"
    }
    $manifestPath = Join-Path $stagingDir 'PAYLOAD-SHA256.txt'
    [System.IO.File]::WriteAllText(
        $manifestPath,
        (($manifestLines -join "`n") + "`n"),
        [System.Text.UTF8Encoding]::new($false))

    Remove-ValidatedDirectory -Path $payloadDir -Parent $resolvedOutputRoot
    Remove-ValidatedDirectory -Path $legacyPayloadDir -Parent $resolvedOutputRoot
    Move-Item -LiteralPath $stagingDir -Destination $payloadDir
    $stagingCreated = $false
}
catch {
    if ($stagingCreated -and (Test-Path -LiteralPath $stagingDir)) {
        Remove-ValidatedDirectory -Path $stagingDir -Parent $resolvedOutputRoot
    }
    throw
}

if (-not $SkipInstaller) {
    & $installerBuildScript `
        -PayloadDir $payloadDir `
        -OutputDir $resolvedOutputRoot `
        -Version $Version `
        -InnoCompiler $InnoCompiler
}

Write-Host "World War VR standalone payload ready: $payloadDir"
if ($SkipInstaller) {
    Write-Host 'Installer build skipped.'
}
else {
    Write-Host ("Installer ready: {0}" -f `
        (Join-Path $resolvedOutputRoot 'WorldWarVR-Setup.exe'))
}
