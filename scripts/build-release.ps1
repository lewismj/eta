#Requires -Version 5.1
<#
.SYNOPSIS
    Build an Eta release bundle on Windows.

.DESCRIPTION
    Configures, builds, and installs Eta binaries + stdlib + VS Code
    extension into a single self-contained directory, then zips it.

    The bundle directory is named  eta-<version>-<platform>  so that
    different releases can live side-by-side.

.PARAMETER InstallDir
    Optional. Directory to install the release bundle into.
    Defaults to  dist\eta-<version>-win-<arch>  under the project root.

.PARAMETER Version
    Optional. Version tag to embed in the bundle name (e.g. "v0.3.0").
    Auto-detected from  git describe --tags --abbrev=0  when omitted,
    falling back to the version in CMakeLists.txt.

.EXAMPLE
    .\scripts\build-release.ps1
    .\scripts\build-release.ps1 -Version v0.3.0
    .\scripts\build-release.ps1 C:\eta-release
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$InstallDir,

    [Parameter()]
    [string]$Version
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

# ── Resolve install dir ──────────────────────────────────────────────
if (-not $InstallDir) {
    $InstallDir = Join-Path $ProjectRoot "dist\eta-$Version-$PlatformTag"
}
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}
$Prefix = (Resolve-Path $InstallDir).Path

Write-Host "╔══════════════════════════════════════════════════════════════╗"
Write-Host "║  Eta Release Build (Windows)                               ║"
Write-Host "╠══════════════════════════════════════════════════════════════╣"
Write-Host "║  Version  : $Version"
Write-Host "║  Platform : $PlatformTag"
Write-Host "║  Install  : $Prefix"
Write-Host "╚══════════════════════════════════════════════════════════════╝"
Write-Host ""

# ── 1. Configure ─────────────────────────────────────────────────────
Write-Host "▸ [1/6] Configuring CMake..."
& cmake -B $BuildDir -DCMAKE_INSTALL_PREFIX="$Prefix" $ProjectRoot
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# ── 2. Build ─────────────────────────────────────────────────────────
Write-Host "▸ [2/6] Building (Release)..."
& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

# ── 3. Install binaries + stdlib ─────────────────────────────────────
Write-Host "▸ [3/6] Installing to $Prefix..."
& cmake --install $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "CMake install failed" }

# ── 4. Build VS Code extension ───────────────────────────────────────
$VscodeSrc  = Join-Path $ProjectRoot "editors\vscode"
$VscodeDest = Join-Path $Prefix "editors\vscode"

Write-Host "▸ [4/6] Building VS Code extension..."
Push-Location $VscodeSrc
try {
    & npm ci --silent
    if ($LASTEXITCODE -ne 0) { throw "npm ci failed" }
    & npm run compile
    if ($LASTEXITCODE -ne 0) { throw "npm compile failed" }
} finally {
    Pop-Location
}

# Copy extension into the bundle
New-Item -ItemType Directory -Force -Path "$VscodeDest\out"      | Out-Null
New-Item -ItemType Directory -Force -Path "$VscodeDest\syntaxes" | Out-Null
New-Item -ItemType Directory -Force -Path "$VscodeDest\bin"      | Out-Null

Copy-Item -Recurse -Force "$VscodeSrc\out\*"                      "$VscodeDest\out\"
Copy-Item -Recurse -Force "$VscodeSrc\syntaxes\*"                 "$VscodeDest\syntaxes\"
Copy-Item -Force          "$VscodeSrc\package.json"                "$VscodeDest\"
Copy-Item -Force          "$VscodeSrc\tsconfig.json"               "$VscodeDest\"
Copy-Item -Force          "$VscodeSrc\language-configuration.json"  "$VscodeDest\"

# Bundle eta_lsp binary into extension
$LspExe = Join-Path $Prefix "bin\eta_lsp.exe"
if (Test-Path $LspExe) {
    Copy-Item -Force $LspExe "$VscodeDest\bin\"
}

# Production npm deps
Push-Location $VscodeDest
try { & npm install --omit=dev --silent 2>$null } catch {} finally { Pop-Location }

# ── 5. Copy helpers + docs ───────────────────────────────────────────
Write-Host "▸ [5/6] Copying install script and docs..."
$helpers = @(
    (Join-Path $ProjectRoot "scripts\install.ps1"),
    (Join-Path $ProjectRoot "scripts\install.cmd"),
    (Join-Path $ProjectRoot "TESTING.md")
)
foreach ($h in $helpers) {
    if (Test-Path $h) { Copy-Item -Force $h "$Prefix\" }
}

# ── 6. Create zip archive ────────────────────────────────────────────
$BundleName  = Split-Path -Leaf $Prefix
$ArchivePath = Join-Path (Split-Path -Parent $Prefix) "$BundleName.zip"

Write-Host "▸ [6/6] Creating archive $BundleName.zip..."
if (Test-Path $ArchivePath) { Remove-Item $ArchivePath }
Compress-Archive -Path $Prefix -DestinationPath $ArchivePath

# ── Done ─────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "════════════════════════════════════════════════════════════════" -ForegroundColor Green
Write-Host "✓ Release bundle ready!" -ForegroundColor Green
Write-Host ""
Write-Host "  Directory : $Prefix"
Write-Host "  Archive   : $ArchivePath"
Write-Host ""
Write-Host "  To install on a target machine:"
Write-Host "    Expand-Archive $BundleName.zip -DestinationPath ."
Write-Host "    cd $BundleName"
Write-Host "    .\install.cmd                        (recommended)"
Write-Host "    .\install.cmd `"C:\Program Files\Eta`"  (with prefix)"
Write-Host "════════════════════════════════════════════════════════════════" -ForegroundColor Green

