[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PayloadDir,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [ValidatePattern('^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-(?:alpha|beta|rc)\.(?:0|[1-9][0-9]*))?$')]
    [string]$Version = '0.4.0-alpha.1',

    [string]$InnoCompiler = '',

    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedInnoCompilerSha256 = '0A8757031B33777E4C9CBFFEE40F11A5062B36D25CBE144C1DB73B6102B80AD7',

    [switch]$RequireSignature
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$setupScript = Join-Path $scriptRoot 'WorldWarVR.iss'
$resolvedPayload = [System.IO.Path]::GetFullPath($PayloadDir)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDir)
$installerPath = Join-Path $resolvedOutput 'WorldWarVR-Setup.exe'
$legacyInstallerPath = Join-Path $resolvedOutput 'WorldAtWarVR-Setup.exe'
$null = $Version -match '^(\d+)\.(\d+)\.(\d+)'
$fileVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
foreach ($component in @($Matches[1], $Matches[2], $Matches[3])) {
    if ([uint64]$component -gt 65535) {
        throw "Version components must fit the Windows file-version range: $Version"
    }
}

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

function Test-IsSameOrDescendant {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Candidate,

        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $fullCandidate = [System.IO.Path]::GetFullPath($Candidate).TrimEnd('\')
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    return $fullCandidate.Equals(
            $fullRoot,
            [StringComparison]::OrdinalIgnoreCase) -or
        $fullCandidate.StartsWith(
            $fullRoot + '\',
            [StringComparison]::OrdinalIgnoreCase)
}

function Resolve-InnoCompiler {
    if (-not [string]::IsNullOrWhiteSpace($InnoCompiler)) {
        $explicitPath = [System.IO.Path]::GetFullPath($InnoCompiler)
        if (-not (Test-Path -LiteralPath $explicitPath -PathType Leaf)) {
            throw "Inno Setup compiler was not found: $explicitPath"
        }
        return $explicitPath
    }

    $command = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
        $candidates += Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
    }
    if (-not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
        $candidates += Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $candidates += Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    throw 'Inno Setup 6 was not found. Install the pinned Inno Setup 6.7.3 compiler or pass -InnoCompiler.'
}

if (-not (Test-Path -LiteralPath $setupScript -PathType Leaf)) {
    throw "Installer definition is missing: $setupScript"
}
if (-not (Test-Path -LiteralPath $resolvedPayload -PathType Container)) {
    throw "Payload directory was not found: $resolvedPayload"
}
Assert-NormalDirectory -Path $resolvedPayload -Description 'Payload directory'
if (Test-IsSameOrDescendant `
        -Candidate $resolvedOutput `
        -Root $resolvedPayload) {
    throw 'Installer output must remain outside the payload directory.'
}

if (Test-Path -LiteralPath $resolvedOutput) {
    Assert-NormalDirectory -Path $resolvedOutput -Description 'Installer output directory'
}
else {
    New-Item -ItemType Directory -Path $resolvedOutput | Out-Null
}

$requiredTopLevelFiles = @(
    'WorldWarVR.exe',
    'WorldWarVR.Launcher.dll',
    'WorldWarVR.Launcher.Core.dll',
    'WorldWarVR.ico',
    'wawvr-launcher.exe',
    'WorldWarVR.dll',
    'WaWVR-PeZBOT-Import.ps1',
    'LICENSE',
    'licenses\OpenXR-SDK\LICENSE.txt',
    'licenses\JsonCpp\LICENSE.txt',
    'licenses\nuget\Microsoft.WindowsAppSDK.ML_1.8.2141\license.txt',
    'licenses\nuget\Microsoft.WindowsAppSDK.ML_1.8.2141\ThirdPartyNotices.txt'
)
foreach ($requiredFile in $requiredTopLevelFiles) {
    $requiredPath = Join-Path $resolvedPayload $requiredFile
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required installer payload file is missing: $requiredFile"
    }
}

$allItems = @(Get-ChildItem -LiteralPath $resolvedPayload -Recurse -Force)
if ($allItems.Count -gt 2048) {
    throw "Installer payload contains too many entries: $($allItems.Count)"
}

$prohibitedNames = @(
    'CoDWaW.exe',
    'CoDWaWmp.exe',
    't4sp.exe',
    't4mp.exe',
    'binkw32.dll',
    'PeZBOTWAW_005p.zip',
    'mod.ff',
    'PeZBOTWaW.iwd',
    'THIRD-PARTY-NOTICES.md',
    'PROVENANCE_AUDIT.md',
    'PROVENANCE-AUDIT.md',
    'components.json',
    'WorldAtWarVR.exe',
    'WorldAtWarVR.dll',
    'WorldAtWarVR.Launcher.dll',
    'WorldAtWarVR.Launcher.Core.dll',
    'WorldAtWarVR.ico',
    'WorldAtWarVR.pri',
    'WorldAtWarVR-Setup.exe',
    'WorldWarVR-Setup.exe'
)
$prohibitedExtensions = @(
    '.bik',
    '.d3dbsp',
    '.dmp',
    '.exp',
    '.ff',
    '.iwd',
    '.lib',
    '.log',
    '.obj',
    '.pdb'
)

$payloadFiles = @()
foreach ($item in $allItems) {
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Installer payload must not contain links or junctions: $($item.FullName)"
    }
    if ($item.PSIsContainer) {
        continue
    }
    $payloadFiles += $item
    if ($prohibitedNames -icontains $item.Name -or
        $item.Name -ilike 'plutonium*' -or
        $prohibitedExtensions -icontains $item.Extension) {
        $relativePath = [System.IO.Path]::GetRelativePath($resolvedPayload, $item.FullName)
        throw "Prohibited game, third-party mod, or build-only file entered the installer payload: $relativePath"
    }
}

if ($payloadFiles.Count -lt $requiredTopLevelFiles.Count) {
    throw 'Installer payload inventory is incomplete.'
}
$payloadBytes = ($payloadFiles | Measure-Object -Property Length -Sum).Sum
if ($payloadBytes -gt 1GB) {
    throw "Installer payload exceeds the 1 GiB safety limit: $payloadBytes bytes"
}

$compilerPath = Resolve-InnoCompiler
$compilerSha256 = (Get-FileHash -LiteralPath $compilerPath -Algorithm SHA256).Hash
if (-not $compilerSha256.Equals(
        $ExpectedInnoCompilerSha256,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Inno Setup compiler hash does not match the pinned 6.7.3 compiler: $compilerSha256"
}

if (Test-Path -LiteralPath $installerPath -PathType Leaf) {
    Remove-Item -LiteralPath $installerPath -Force
}
if (Test-Path -LiteralPath $legacyInstallerPath -PathType Leaf) {
    Remove-Item -LiteralPath $legacyInstallerPath -Force
}

& $compilerPath `
    "/DPayloadDir=$resolvedPayload" `
    "/DInstallerOutputDir=$resolvedOutput" `
    "/DProductVersion=$Version" `
    "/DProductFileVersion=$fileVersion" `
    $setupScript
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "Inno Setup completed without producing the expected file: $installerPath"
}

$signature = Get-AuthenticodeSignature -LiteralPath $installerPath
if ($RequireSignature -and $signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
    throw "The installer is not validly Authenticode-signed: $($signature.Status)"
}

$installer = Get-Item -LiteralPath $installerPath
$sha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
Write-Host "World War VR installer ready: $installerPath"
Write-Host "Size: $($installer.Length) bytes"
Write-Host "SHA-256: $sha256"
Write-Host "Authenticode: $($signature.Status)"
