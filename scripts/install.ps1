#Requires -Version 5.1
<#
.SYNOPSIS
    Install Eta from an extracted release bundle on Windows.

.DESCRIPTION
    Adds Eta's bin/ to the user PATH, sets ETA_MODULE_PATH, and
    optionally installs the VS Code extension.

    When called with no argument the bundle directory itself is used.
    When called with a -Prefix, files are copied to that location first.

    NOTE: If your execution policy blocks this script, use the bundled
    install.cmd wrapper instead (it calls this script with
    -ExecutionPolicy Bypass).

.PARAMETER Prefix
    Optional. Copy bin/, stdlib/, editors/ to this directory and
    configure PATH to point there instead of the bundle location.

.EXAMPLE
    cd eta-win-x64
    .\install.cmd

.EXAMPLE
    .\install.cmd "C:\Program Files\Eta"
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Prefix
)

$ErrorActionPreference = "Stop"

$BundleDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# -- Copy to prefix if given ---------------------------------------------------
if ($Prefix) {
    Write-Host "> Copying files to $Prefix..."
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
    $VsixPath  = "$Prefix\editors\eta-lang.vsix"
} else {
    $BinDir    = Join-Path $BundleDir "bin"
    $StdlibDir = Join-Path $BundleDir "stdlib"
    $VsixPath  = Join-Path $BundleDir "editors\eta-lang.vsix"
}

Write-Host "+==============================================================+"
Write-Host "|  Eta Installer (Windows)                                     |"
Write-Host "+==============================================================+"
Write-Host ""
Write-Host "  bin     : $BinDir"
Write-Host "  stdlib  : $StdlibDir"
Write-Host ""

# -- 1. Add bin/ to user PATH -------------------------------------------------
$UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($UserPath -notlike "*$BinDir*") {
    Write-Host "> Adding $BinDir to user PATH..."
    $NewPath = "$BinDir;$UserPath"
    [Environment]::SetEnvironmentVariable("PATH", $NewPath, "User")
    $env:PATH = "$BinDir;$env:PATH"
    Write-Host "  [OK] Added to user PATH."
} else {
    Write-Host "> Eta already on user PATH -- skipping."
}

# -- 2. Set ETA_MODULE_PATH ---------------------------------------------------
$CurrentModPath = [Environment]::GetEnvironmentVariable("ETA_MODULE_PATH", "User")
if ($CurrentModPath -ne $StdlibDir) {
    Write-Host "> Setting ETA_MODULE_PATH = $StdlibDir..."
    [Environment]::SetEnvironmentVariable("ETA_MODULE_PATH", $StdlibDir, "User")
    $env:ETA_MODULE_PATH = $StdlibDir
    Write-Host "  [OK] Set."
} else {
    Write-Host "> ETA_MODULE_PATH already set -- skipping."
}

# -- 3. Install VS Code extension ---------------------------------------------
$CodeExe = Get-Command code -ErrorAction SilentlyContinue

if ($CodeExe -and (Test-Path $VsixPath)) {
    Write-Host "> Installing VS Code extension..."
    & code --install-extension $VsixPath --force
    Write-Host "  [OK] VS Code extension installed."
} elseif (-not (Test-Path $VsixPath)) {
    Write-Host "> VS Code extension not in bundle -- skipping."
    Write-Host "    Set eta.lsp.serverPath in VS Code settings to:"
    Write-Host "    $BinDir\eta_lsp.exe"
    Write-Host "    Set eta.dap.executablePath in VS Code settings to:"
    Write-Host "    $BinDir\eta_dap.exe"
} else {
    Write-Host "> 'code' not on PATH -- skipping VS Code extension install."
    Write-Host "    To install manually: code --install-extension `"$VsixPath`" --force"
}

# -- 4. Smoke test -------------------------------------------------------------
Write-Host ""
Write-Host "> Verifying..."
foreach ($bin in @("etai.exe", "eta_repl.exe", "eta_lsp.exe", "eta_dap.exe")) {
    $p = Join-Path $BinDir $bin
    if (Test-Path $p) {
        Write-Host "  [OK] $bin"
    } else {
        Write-Host "  [FAIL] $bin -- not found"
    }
}

Write-Host ""
Write-Host "[OK] Done! Open a new terminal and try:" -ForegroundColor Green
Write-Host ""
Write-Host "    etai --help"
Write-Host "    eta_repl"
Write-Host ""

