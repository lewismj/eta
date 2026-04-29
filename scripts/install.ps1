#Requires -Version 5.1
<#
.SYNOPSIS
    Install Eta from an extracted release bundle on Windows.

.DESCRIPTION
    Adds Eta's bin/ to the user PATH (removing any stale Eta entries from
    previous installs), sets ETA_MODULE_PATH, and optionally installs the
    VS Code extension.

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
    if (Test-Path "$BundleDir\lib") {
        New-Item -ItemType Directory -Force -Path "$Prefix\lib" | Out-Null
        Copy-Item -Recurse -Force "$BundleDir\lib\*" "$Prefix\lib\"
    }
    if (Test-Path "$BundleDir\editors") {
        New-Item -ItemType Directory -Force -Path "$Prefix\editors" | Out-Null
        Copy-Item -Recurse -Force "$BundleDir\editors\*" "$Prefix\editors\"
    }

    $BinDir    = (Resolve-Path "$Prefix\bin").Path
    $StdlibDir = (Resolve-Path "$Prefix\stdlib").Path
    $EditorsDir = "$Prefix\editors"
} else {
    $BinDir     = Join-Path $BundleDir "bin"
    $StdlibDir  = Join-Path $BundleDir "stdlib"
    $EditorsDir = Join-Path $BundleDir "editors"
}

# Resolve the VS Code extension. The build-release script produces a
# versioned filename like `eta-lang-0.3.0.vsix`; older bundles used the
# unversioned `eta-lang.vsix`. Accept either, preferring the most recent
# versioned match if multiple are present.
$VsixPath = $null
if (Test-Path $EditorsDir) {
    $vsix = Get-ChildItem -Path $EditorsDir -Filter 'eta-lang-*.vsix' -File `
        -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($vsix) {
        $VsixPath = $vsix.FullName
    } elseif (Test-Path (Join-Path $EditorsDir 'eta-lang.vsix')) {
        $VsixPath = Join-Path $EditorsDir 'eta-lang.vsix'
    }
}

Write-Host "+==============================================================+"
Write-Host "|  Eta Installer (Windows)                                     |"
Write-Host "+==============================================================+"
Write-Host ""
Write-Host "  bin     : $BinDir"
Write-Host "  stdlib  : $StdlibDir"
Write-Host ""

# -- 1. Add bin/ to user PATH, removing any stale Eta entries -----------------
$UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")

# Split into individual entries and drop any that already contain Eta binaries
# (i.e. old installs pointing to a different directory).
$EtaMarker = "etai.exe"
$CleanEntries = ($UserPath -split ';') | Where-Object {
    $entry = $_.Trim()
    if ($entry -eq '')      { return $false }   # drop empty segments
    if ($entry -eq $BinDir) { return $false }   # will be re-added at front
    $hasMarker = Test-Path (Join-Path $entry $EtaMarker)
    if ($hasMarker) {
        Write-Host "> Removing stale Eta PATH entry: $entry"
    }
    return -not $hasMarker
}

$NewPath = if ($CleanEntries) { "$BinDir;$($CleanEntries -join ';')" } else { $BinDir }

if ($NewPath -ne $UserPath) {
    Write-Host "> Updating user PATH with $BinDir..."
    [Environment]::SetEnvironmentVariable("PATH", $NewPath, "User")
    $env:PATH = "$BinDir;$env:PATH"
    Write-Host "  [OK] PATH updated."
} else {
    Write-Host "> Eta already on user PATH at correct location -- skipping."
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
foreach ($bin in @("etac.exe", "etai.exe", "eta_repl.exe", "eta_lsp.exe", "eta_dap.exe", "eta_jupyter.exe")) {
    $p = Join-Path $BinDir $bin
    if (Test-Path $p) {
        Write-Host "  [OK] $bin"
    } else {
        Write-Host "  [FAIL] $bin -- not found"
    }
}

Write-Host ""
Write-Host "> Jupyter kernel setup:"
$EtaJupyterExe = Join-Path $BinDir "eta_jupyter.exe"
if (Test-Path $EtaJupyterExe) {
    # Sanity-check that the xeus shared libraries shipped alongside the
    # executable. Missing DLLs surface as STATUS_DLL_NOT_FOUND (-1073741515 /
    # 0xC0000135) when eta_jupyter is launched, which is otherwise opaque.
    # The DLL names vary by build: MSVC FetchContent produces libxeus.dll /
    # libxeus-zmq.dll; vcpkg / conda produce xeus.dll / xeus-zmq.dll.
    # uv.dll (libuv) is a direct runtime dependency of xeus-zmq 4.x.
    $HasXeus    = (Test-Path (Join-Path $BinDir "xeus.dll")) -or
                  (Test-Path (Join-Path $BinDir "libxeus.dll"))
    $HasXeusZmq = (Test-Path (Join-Path $BinDir "xeus-zmq.dll")) -or
                  (Test-Path (Join-Path $BinDir "libxeus-zmq.dll"))
    $HasAnyZmq  = (Test-Path (Join-Path $BinDir "libzmq.dll")) -or
                  [bool](Get-ChildItem -Path $BinDir -Filter "libzmq*.dll" -File -ErrorAction SilentlyContinue | Select-Object -First 1)
    $HasUv      = (Test-Path (Join-Path $BinDir "uv.dll"))

    if (-not ($HasXeus -and $HasXeusZmq -and $HasAnyZmq -and $HasUv)) {
        Write-Host "  [WARN] Missing xeus runtime DLLs in $BinDir :" -ForegroundColor Yellow
        if (-not $HasXeus)    { Write-Host "         - xeus.dll / libxeus.dll" }
        if (-not $HasXeusZmq) { Write-Host "         - xeus-zmq.dll / libxeus-zmq.dll" }
        if (-not $HasAnyZmq)  { Write-Host "         - libzmq*.dll" }
        if (-not $HasUv)      { Write-Host "         - uv.dll  (libuv -- required by xeus-zmq 4.x)" }
        Write-Host "         eta_jupyter will fail to start with STATUS_DLL_NOT_FOUND."
        Write-Host "         Rebuild from source (this bundle is missing required runtime DLLs)."
    }

    Write-Host "  Installing Eta kernelspec (--user)..."
    try {
        & $EtaJupyterExe --install --user
        if ($LASTEXITCODE -ne 0) {
            throw "eta_jupyter exited with code $LASTEXITCODE"
        }
        Write-Host "  [OK] Kernel installed."
    } catch {
        Write-Host "  [WARN] Kernel auto-install failed: $($_.Exception.Message)"
        if ($LASTEXITCODE -eq -1073741515) {
            Write-Host "         Exit code -1073741515 (0xC0000135) = STATUS_DLL_NOT_FOUND." -ForegroundColor Yellow
            Write-Host "         eta_jupyter.exe is missing one or more required DLLs."
            Write-Host "         Required: xeus / libxeus, xeus-zmq / libxeus-zmq, libzmq*.dll, uv.dll"
        }
        Write-Host "    Run manually: `"$EtaJupyterExe`" --install --user"
    }
    $JupyterCmd = Get-Command jupyter -ErrorAction SilentlyContinue
    if (-not $JupyterCmd) {
        Write-Host "    python -m pip install jupyterlab"
    }
    Write-Host "    jupyter lab"
} else {
    Write-Host "    eta_jupyter.exe not found in $BinDir; kernel install unavailable."
}

Write-Host ""
Write-Host "[OK] Done! Open a new terminal and try:" -ForegroundColor Green
Write-Host ""
Write-Host "    etai --help"
Write-Host "    eta_repl"
Write-Host ""
