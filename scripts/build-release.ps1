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
    .\scripts\build-release.cmd -EnableTorch -TorchBackend cu126
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
    [ValidateSet("cpu", "cu126", "cu128", "cu130")]
    [string]$TorchBackend = "cpu"
)

$ErrorActionPreference = "Stop"

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = (Resolve-Path "$ScriptDir\..").Path
$BuildDir    = Join-Path $ProjectRoot "build-release"

function Copy-VsRuntimeDllsToBin {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$BinDir
    )

    $CachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path $CachePath)) {
        Write-Host "  [WARN] CMakeCache.txt not found; skipping MSVC runtime DLL copy." -ForegroundColor Yellow
        return
    }

    $CompilerLine = Get-Content $CachePath | Where-Object { $_ -like "CMAKE_CXX_COMPILER:FILEPATH=*" } | Select-Object -First 1
    if (-not $CompilerLine) {
        Write-Host "  [WARN] CMAKE_CXX_COMPILER not found in cache; skipping MSVC runtime DLL copy." -ForegroundColor Yellow
        return
    }

    $CompilerPath = ($CompilerLine -split "=", 2)[1]
    $CompilerPath = [System.IO.Path]::GetFullPath(($CompilerPath -replace '/', '\'))
    if ($CompilerPath -notmatch "^(?<vcroot>.+\\VC)\\Tools\\MSVC\\(?<ver>[^\\]+)\\bin\\") {
        Write-Host "  [WARN] Could not derive VC redist path from compiler '$CompilerPath'; skipping runtime DLL copy." -ForegroundColor Yellow
        return
    }

    $VcRoot = $Matches["vcroot"]
    $MsvcVer = $Matches["ver"]
    $RedistBase = Join-Path $VcRoot "Redist\MSVC"
    if (-not (Test-Path $RedistBase)) {
        Write-Host "  [WARN] VC redist base not found at '$RedistBase'; skipping runtime DLL copy." -ForegroundColor Yellow
        return
    }

    $CandidateRoots = New-Object System.Collections.Generic.List[string]
    $CandidateRoots.Add((Join-Path $RedistBase $MsvcVer))
    Get-ChildItem -Path $RedistBase -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' -and $_.Name -ne $MsvcVer } |
        Sort-Object Name -Descending |
        ForEach-Object { $CandidateRoots.Add($_.FullName) }

    foreach ($Root in $CandidateRoots) {
        $X64Dir = Join-Path $Root "x64"
        if (-not (Test-Path $X64Dir)) { continue }

        $CrtDir = Get-ChildItem -Path $X64Dir -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $CrtDir) { continue }

        $Dlls = Get-ChildItem -Path $CrtDir.FullName -File -Filter "*.dll" -ErrorAction SilentlyContinue
        if (-not $Dlls) { continue }

        foreach ($Dll in $Dlls) {
            Copy-Item -Force $Dll.FullName (Join-Path $BinDir $Dll.Name)
        }
        Write-Host "  Copied $($Dlls.Count) MSVC runtime DLL(s) from '$($CrtDir.FullName)'"
        return
    }

    Write-Host "  [WARN] No Microsoft.VC*.CRT runtime directory found under '$RedistBase'; skipping runtime DLL copy." -ForegroundColor Yellow
}

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

# Verify required runtime binaries are present in the bundle.
Write-Host "  Verifying runtime binaries..."
foreach ($bin in @("eta.exe", "etac.exe", "etai.exe", "eta_test.exe", "eta_repl.exe", "eta_lsp.exe", "eta_dap.exe", "eta_jupyter.exe")) {
    $p = Join-Path $Prefix "bin\$bin"
    if (-not (Test-Path $p)) {
        throw "Missing required binary after install: $p"
    }
}

# Verify stdlib source + precompiled artifacts landed in the bundle.
Write-Host "  Verifying stdlib artifacts..."
$PreludeEta = Join-Path $Prefix "stdlib\std\prelude.eta"
if (-not (Test-Path $PreludeEta)) {
    throw "Missing required stdlib source after install: $PreludeEta"
}
$PreludeEtac = Join-Path $Prefix "stdlib\std\prelude.etac"
if (-not (Test-Path $PreludeEtac)) {
    throw "Missing required stdlib bytecode after install: $PreludeEtac"
}

# Bundle the MSVC runtime DLLs into bin/ for clean-machine execution.
$BinPath = Join-Path $Prefix "bin"
Copy-VsRuntimeDllsToBin -BuildDir $BuildDir -BinDir $BinPath

# spdlog is required by std.log at runtime when built as a shared library.
$HasSpdlog = [bool](Get-ChildItem -Path $BinPath -Filter "spdlog*.dll" -File -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not $HasSpdlog) {
    Write-Host "  [WARN] spdlog runtime DLL missing from bin\\ -- std.log features may fail at runtime:" -ForegroundColor Yellow
    Write-Host "         - spdlog*.dll"
}

# Verify xeus runtime DLLs landed in bin/ — without these eta_jupyter.exe
# fails to start with STATUS_DLL_NOT_FOUND (0xC0000135 / -1073741515).
# The DLL names vary by build: MSVC FetchContent produces libxeus.dll /
# libxeus-zmq.dll; vcpkg / conda produce xeus.dll / xeus-zmq.dll.
# uv.dll / libuv*.dll (libuv) is a direct runtime dependency of xeus-zmq 4.x.
$HasXeus    = (Test-Path (Join-Path $BinPath "xeus.dll")) -or
              (Test-Path (Join-Path $BinPath "libxeus.dll"))
$HasXeusZmq = (Test-Path (Join-Path $BinPath "xeus-zmq.dll")) -or
              (Test-Path (Join-Path $BinPath "libxeus-zmq.dll"))
$HasZmq     = [bool](
                (Get-ChildItem -Path $BinPath -Filter "libzmq*.dll" -File -ErrorAction SilentlyContinue | Select-Object -First 1) -or
                (Get-ChildItem -Path $BinPath -Filter "zmq*.dll" -File -ErrorAction SilentlyContinue | Select-Object -First 1)
              )
$HasUv      = (Test-Path (Join-Path $BinPath "uv.dll")) -or
              [bool](Get-ChildItem -Path $BinPath -Filter "libuv*.dll" -File -ErrorAction SilentlyContinue | Select-Object -First 1)
$HasCrypto  = [bool](Get-ChildItem -Path $BinPath -Filter "libcrypto*.dll" -File -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not ($HasXeus -and $HasXeusZmq -and $HasZmq -and $HasUv)) {
    Write-Host "  [WARN] xeus runtime DLLs missing from bin\ -- eta_jupyter will not run on a clean machine:" -ForegroundColor Yellow
    if (-not $HasXeus)    { Write-Host "         - xeus.dll / libxeus.dll" }
    if (-not $HasXeusZmq) { Write-Host "         - xeus-zmq.dll / libxeus-zmq.dll" }
    if (-not $HasZmq)     { Write-Host "         - libzmq*.dll / zmq*.dll" }
    if (-not $HasUv)      { Write-Host "         - uv.dll / libuv*.dll  (libuv -- required by xeus-zmq 4.x)" }
}
if (-not $HasCrypto) {
    Write-Host "  [WARN] libcrypto*.dll not found in bin\ -- this is only required when libzmq/xeus-zmq is built with OpenSSL/CURVE support." -ForegroundColor Yellow
}

# Keep Jupyter kernelspec logos next to eta_jupyter so `eta_jupyter --install`
# can copy them on target machines (without source-tree paths).
$JupyterResSrc = Join-Path $ProjectRoot "eta\tools\jupyter\resources"
if (Test-Path $JupyterResSrc) {
    $JupyterResDest = Join-Path $Prefix "bin\resources"
    New-Item -ItemType Directory -Force -Path $JupyterResDest | Out-Null
    foreach ($logo in @("logo-32x32.png", "logo-64x64.png")) {
        $src = Join-Path $JupyterResSrc $logo
        if (Test-Path $src) {
            Copy-Item -Force $src (Join-Path $JupyterResDest $logo)
        }
    }
}

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

# Derive semver from Version (strip leading 'v'), or fall back to "latest"
$Semver = $Version -replace '^v',''
if ($Semver -match '^\d+\.\d+\.\d+') {
    $VsixLabel = $Semver
} else {
    $VsixLabel = "latest"
}
$VsixDest = Join-Path $EditorsDir "eta-lang-${VsixLabel}.vsix"

Write-Host "> [4/6] Building VS Code extension ($VsixLabel)..."
New-Item -ItemType Directory -Force -Path $EditorsDir | Out-Null

Push-Location $VscodeSrc
try {
    & npm ci
    if ($LASTEXITCODE -ne 0) { throw "npm ci failed" }
    & npm run bundle
    if ($LASTEXITCODE -ne 0) { throw "npm run bundle failed" }
    if ($VsixLabel -ne "latest") {
        & npm version $VsixLabel --no-git-tag-version --allow-same-version
        if ($LASTEXITCODE -ne 0) { throw "npm version failed" }
    }
    & npx vsce package -o $VsixDest --skip-license
    if ($LASTEXITCODE -ne 0) { throw "vsce package failed" }
} finally {
    Pop-Location
}


# -- 5. Copy helpers + docs ----------------------------------------------------
Write-Host "> [5/6] Copying install script, docs, and cookbook..."
$helpers = @(
    (Join-Path $ProjectRoot "scripts\install.ps1"),
    (Join-Path $ProjectRoot "scripts\install.cmd"),
    (Join-Path $ProjectRoot "docs\quickstart.md")
)
foreach ($h in $helpers) {
    if (Test-Path $h) { Copy-Item -Force $h "$Prefix\" }
}

# Copy cookbook/
$CookbookSrc  = Join-Path $ProjectRoot "cookbook"
$CookbookDest = Join-Path $Prefix "cookbook"
if (Test-Path $CookbookSrc) {
    Write-Host "  Copying cookbook..."
    Copy-Item -Recurse -Force $CookbookSrc $CookbookDest
}

# -- 5b. Prune to minimal Windows layout ---------------------------------------
# On Windows everything runtime-related lives in bin\ (executables + DLLs
# copied next to them).  Drop any include/, lib/, share/ trees that
# third-party dependencies may have installed despite EXCLUDE_FROM_ALL.
Write-Host "  Pruning non-essential install directories..."
$keepNames = @('bin','editors','stdlib','cookbook')
Get-ChildItem -LiteralPath $Prefix -Directory | Where-Object { $keepNames -notcontains $_.Name } | ForEach-Object {
    Write-Host "    - removing $($_.Name)\"
    Remove-Item -LiteralPath $_.FullName -Recurse -Force
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
