#Requires -Version 5.1
<#
.SYNOPSIS
    Install Eta from an extracted release bundle on Windows.

.DESCRIPTION
    Adds Eta's bin/ to the user PATH, sets ETA_MODULE_PATH, and
    optionally installs the VS Code extension.

    When called with no argument the bundle directory itself is used.
    When called with a -Prefix, files are copied to that location first.

.PARAMETER Prefix
    Optional. Copy bin/, stdlib/, editors/ to this directory and
    configure PATH to point there instead of the bundle location.

.EXAMPLE
    cd eta-win-x64
    .\install.ps1

.EXAMPLE
    .\install.ps1 -Prefix C:\Program Files\Eta
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Prefix
)

$ErrorActionPreference = "Stop"

$BundleDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# ── Copy to prefix if given ───────────────────────────────────────────
if ($Prefix) {
    Write-Host "▸ Copying files to $Prefix..."
    New-Item -ItemType Directory -Force -Path "$Prefix\bin"    | Out-Null
    New-Item -ItemType Directory -Force -Path "$Prefix\stdlib" | Out-Null

    Copy-Item -Recurse -Force "$BundleDir\bin\*"    "$Prefix\bin\"
    Copy-Item -Recurse -Force "$BundleDir\stdlib\*" "$Prefix\stdlib\"
    if (Test-Path "$BundleDir\editors") {
        New-Item -ItemType Directory -Force -Path "$Prefix\editors" | Out-Null
        Copy-Item -Recurse -Force "$BundleDir\editors\*" "$Prefix\editors\"
    }

    $BinDir    = (Resolve-Path "$Prefix\bin").Path
    $StdlibDir = (Resolve-Path "$Prefix\stdlib").Path
    $VscodeDir = "$Prefix\editors\vscode"
} else {
    $BinDir    = Join-Path $BundleDir "bin"
    $StdlibDir = Join-Path $BundleDir "stdlib"
    $VscodeDir = Join-Path $BundleDir "editors\vscode"
}

Write-Host "╔══════════════════════════════════════════════════════════════╗"
Write-Host "║  Eta Installer (Windows)                                   ║"
Write-Host "╚══════════════════════════════════════════════════════════════╝"
Write-Host ""
Write-Host "  bin     : $BinDir"
Write-Host "  stdlib  : $StdlibDir"
Write-Host ""

# ── 1. Add bin/ to user PATH ─────────────────────────────────────────
$UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($UserPath -notlike "*$BinDir*") {
    Write-Host "▸ Adding $BinDir to user PATH..."
    $NewPath = "$BinDir;$UserPath"
    [Environment]::SetEnvironmentVariable("PATH", $NewPath, "User")
    $env:PATH = "$BinDir;$env:PATH"
    Write-Host "  ✓ Added to user PATH."
} else {
    Write-Host "▸ Eta already on user PATH — skipping."
}

# ── 2. Set ETA_MODULE_PATH ───────────────────────────────────────────
$CurrentModPath = [Environment]::GetEnvironmentVariable("ETA_MODULE_PATH", "User")
if ($CurrentModPath -ne $StdlibDir) {
    Write-Host "▸ Setting ETA_MODULE_PATH = $StdlibDir..."
    [Environment]::SetEnvironmentVariable("ETA_MODULE_PATH", $StdlibDir, "User")
    $env:ETA_MODULE_PATH = $StdlibDir
    Write-Host "  ✓ Set."
} else {
    Write-Host "▸ ETA_MODULE_PATH already set — skipping."
}

# ── 3. Install VS Code extension ─────────────────────────────────────
$CodeExe = Get-Command code -ErrorAction SilentlyContinue
if ($CodeExe -and (Test-Path $VscodeDir)) {
    Write-Host "▸ Installing VS Code extension..."

    $VsixTmp = Join-Path $env:TEMP "eta-lang.vsix"
    $Packed  = $false
    $NpxExe  = Get-Command npx -ErrorAction SilentlyContinue

    if ($NpxExe) {
        Push-Location $VscodeDir
        try {
            & npx @vscode/vsce package -o $VsixTmp --skip-license 2>$null
            if ($LASTEXITCODE -eq 0) { $Packed = $true }
        } catch {} finally { Pop-Location }
    }

    if ($Packed -and (Test-Path $VsixTmp)) {
        & code --install-extension $VsixTmp --force
        Remove-Item $VsixTmp -ErrorAction SilentlyContinue
        Write-Host "  ✓ VS Code extension installed."
    } else {
        Write-Host "  ⚠ Could not package .vsix (npx/@vscode/vsce not found)."
        Write-Host "    Set eta.lsp.serverPath in VS Code settings to:"
        Write-Host "    $BinDir\eta_lsp.exe"
    }
} elseif (-not (Test-Path $VscodeDir)) {
    Write-Host "▸ VS Code extension not in bundle — skipping."
} else {
    Write-Host "▸ 'code' not on PATH — skipping VS Code extension install."
}

# ── 4. Smoke test ─────────────────────────────────────────────────────
Write-Host ""
Write-Host "▸ Verifying..."
foreach ($bin in @("etai.exe", "eta_repl.exe", "eta_lsp.exe")) {
    $p = Join-Path $BinDir $bin
    if (Test-Path $p) {
        Write-Host "  ✓ $bin"
    } else {
        Write-Host "  ✗ $bin — not found"
    }
}

Write-Host ""
Write-Host "✓ Done! Open a new terminal and try:" -ForegroundColor Green
Write-Host ""
Write-Host "    etai --help"
Write-Host "    eta_repl"
Write-Host ""

