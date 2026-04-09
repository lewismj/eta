#Requires -Version 5.1
<#
.SYNOPSIS
    Build an Eta release bundle on Windows.

.DESCRIPTION
    Configures, builds, and installs Eta binaries + stdlib + VS Code
    extension into a single self-contained directory, then zips it.

    The bundle directory is named  eta-<version>-<platform>  so that
    different releases can live side-by-side.

    Boost is required. Supply -VcpkgRoot (or set the VCPKG_ROOT
    environment variable) to let CMake find Boost via vcpkg.

.PARAMETER InstallDir
    Optional. Directory to install the release bundle into.
    Defaults to  dist\eta-<version>-win-<arch>  under the project root.

.PARAMETER Version
    Optional. Version tag to embed in the bundle name (e.g. "v0.3.0").
    Auto-detected from  git describe --tags --abbrev=0  when omitted,
    falling back to the version in CMakeLists.txt.

.PARAMETER VcpkgRoot
    Optional. Path to the vcpkg root (the directory containing
    bootstrap-vcpkg.bat).  Falls back to the VCPKG_ROOT environment
    variable when omitted.  When set, passes
    -DCMAKE_TOOLCHAIN_FILE=<VcpkgRoot>\scripts\buildsystems\vcpkg.cmake
    to CMake so Boost is located automatically.

.EXAMPLE
    .\scripts\build-release.cmd
    .\scripts\build-release.cmd -Version v0.3.0
    .\scripts\build-release.cmd -VcpkgRoot C:\src\vcpkg
    .\scripts\build-release.cmd "C:\eta-release" -VcpkgRoot C:\src\vcpkg
    .\scripts\build-release.cmd -EnableTorch
    .\scripts\build-release.cmd -EnableTorch -TorchBackend cu124
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$InstallDir,

    [Parameter()]
    [string]$Version,

    [Parameter()]
    [string]$VcpkgRoot,

    [Parameter()]
    [switch]$EnableTorch,

    [Parameter()]
    [ValidateSet("cpu", "cu118", "cu121", "cu124")]
    [string]$TorchBackend = "cpu"
)

$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = (Resolve-Path "$ScriptDir\..").Path
$BuildDir    = Join-Path $ProjectRoot "build-release"

# ── Detect platform ──────────────────────────────────────────────────
$Arch = if ([Environment]::Is64BitOperatingSystem) { "x64" } else { "x86" }
$PlatformTag = "win-$Arch"

# ── Resolve version tag ──────────────────────────────────────────────
if (-not $Version) {
    # Try git tag first
    $GitExe = Get-Command git -ErrorAction SilentlyContinue
    if ($GitExe) {
        try {
            $Version = & git -C $ProjectRoot describe --tags --abbrev=0 2>$null
        } catch {}
    }
}
if (-not $Version) {
    # Fall back to VERSION in CMakeLists.txt
    $CML = Get-Content "$ProjectRoot\CMakeLists.txt" -Raw
    if ($CML -match 'project\s*\(\s*eta\s+VERSION\s+([\d.]+)') {
        $Version = $Matches[1]
    } else {
        $Version = "unknown"
    }
}

# ── Resolve vcpkg root ───────────────────────────────────────────────
if (-not $VcpkgRoot -and $env:VCPKG_ROOT) {
    $VcpkgRoot = $env:VCPKG_ROOT
}
$ToolchainArg = @()
if ($VcpkgRoot) {
    $ToolchainFile = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $ToolchainFile)) {
        throw "vcpkg toolchain not found at: $ToolchainFile"
    }
    $ToolchainArg = @("-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile")
}

$TorchArg = @()
if ($EnableTorch) {
    $TorchArg = @("-DETA_BUILD_TORCH=ON", "-DETA_TORCH_BACKEND=$TorchBackend")
}

# ── Resolve install dir ──────────────────────────────────────────────
if (-not $InstallDir) {
    $InstallDir = Join-Path $ProjectRoot "dist\eta-$Version-$PlatformTag"
}
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}
$Prefix = (Resolve-Path $InstallDir).Path

Write-Host "+==============================================================+"
Write-Host "|  Eta Release Build (Windows)                                 |"
Write-Host "+==============================================================+"
Write-Host "|  Version  : $Version"
Write-Host "|  Platform : $PlatformTag"
Write-Host "|  Install  : $Prefix"
if ($VcpkgRoot) {
Write-Host "|  vcpkg    : $VcpkgRoot"
}
if ($EnableTorch) {
Write-Host "|  Torch    : Enabled ($TorchBackend)"
}
Write-Host "+==============================================================+"
Write-Host ""

# -- 1. Configure --------------------------------------------------------------
Write-Host "> [1/6] Configuring CMake..."
& cmake -B $BuildDir -DCMAKE_INSTALL_PREFIX="$Prefix" @ToolchainArg @TorchArg $ProjectRoot
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# -- 2. Build ------------------------------------------------------------------
Write-Host "> [2/6] Building (Release)..."
& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

# -- 3. Install binaries + stdlib ----------------------------------------------
Write-Host "> [3/6] Installing to $Prefix..."
& cmake --install $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "CMake install failed" }

# -- 4. Build VS Code extension ------------------------------------------------
# @vscode/vsce requires Node >=20.18.1 (via cheerio/undici transitive deps).
$NodeExe = Get-Command node -ErrorAction SilentlyContinue
if (-not $NodeExe) { throw "Node.js not found on PATH -- required to build the VS Code extension." }
$NodeVersion = & node -e "process.stdout.write(process.version)"
# Strip leading 'v' and split into parts
$nvParts = $NodeVersion.TrimStart('v').Split('.')
[int]$nvMajor = $nvParts[0]; [int]$nvMinor = $nvParts[1]; [int]$nvPatch = $nvParts[2]
$MinMajor = 20; $MinMinor = 18; $MinPatch = 1
$tooOld = ($nvMajor -lt $MinMajor) -or
          ($nvMajor -eq $MinMajor -and $nvMinor -lt $MinMinor) -or
          ($nvMajor -eq $MinMajor -and $nvMinor -eq $MinMinor -and $nvPatch -lt $MinPatch)
if ($tooOld) {
    throw "Node.js $NodeVersion is too old. @vscode/vsce requires >=20.18.1. Please upgrade Node.js."
}
Write-Host "  Node.js $NodeVersion -- OK"

$VscodeSrc  = Join-Path $ProjectRoot "editors\vscode"
$EditorsDir = Join-Path $Prefix "editors"
$VsixDest   = Join-Path $EditorsDir "eta-lang.vsix"

Write-Host "> [4/6] Building VS Code extension..."
New-Item -ItemType Directory -Force -Path $EditorsDir | Out-Null

Push-Location $VscodeSrc
try {
    & npm ci
    if ($LASTEXITCODE -ne 0) { throw "npm ci failed" }
    & npx vsce package -o $VsixDest --skip-license
    if ($LASTEXITCODE -ne 0) { throw "vsce package failed" }
} finally {
    Pop-Location
}


# -- 5. Copy helpers + docs ----------------------------------------------------
Write-Host "> [5/6] Copying install script and docs..."
$helpers = @(
    (Join-Path $ProjectRoot "scripts\install.ps1"),
    (Join-Path $ProjectRoot "scripts\install.cmd"),
    (Join-Path $ProjectRoot "TESTING.md")
)
foreach ($h in $helpers) {
    if (Test-Path $h) { Copy-Item -Force $h "$Prefix\" }
}

# -- 6. Create zip archive -----------------------------------------------------
$BundleName  = Split-Path -Leaf $Prefix
$ArchivePath = Join-Path (Split-Path -Parent $Prefix) "$BundleName.zip"

Write-Host "> [6/6] Creating archive $BundleName.zip..."
if (Test-Path $ArchivePath) { Remove-Item $ArchivePath }
Compress-Archive -Path $Prefix -DestinationPath $ArchivePath

# -- Done ----------------------------------------------------------------------
Write-Host ""
Write-Host "================================================================" -ForegroundColor Green
Write-Host "[OK] Release bundle ready!" -ForegroundColor Green
Write-Host ""
Write-Host "  Directory : $Prefix"
Write-Host "  Archive   : $ArchivePath"
Write-Host ""
Write-Host "  To install on a target machine:"
Write-Host "    Expand-Archive $BundleName.zip -DestinationPath ."
Write-Host "    cd $BundleName"
Write-Host "    .\install.cmd                        (recommended)"
Write-Host "    .\install.cmd `"C:\Program Files\Eta`"  (with prefix)"
Write-Host "================================================================" -ForegroundColor Green
