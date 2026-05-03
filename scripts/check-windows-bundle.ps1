<#
.SYNOPSIS
    Validate (and optionally hydrate) a Windows Eta install bundle.

.DESCRIPTION
    Mirrors the "Assemble bundle" step of .github/workflows/release.yml so the
    same checks can be run locally against a `cmake --install` prefix without
    pushing to CI.

    Performs four things:
      1. Verifies the required executables exist in <Prefix>\bin\.
      2. Hydrates the eta_jupyter runtime DLL set into <Prefix>\bin\ from
         vcpkg's installed bin (flat) and from build/_deps (recursive),
         covering xeus / xeus-zmq / libzmq / libuv / OpenSSL / libsodium.
         CMake's in-tree install rules already copy these when the relevant
         FetchContent / imported targets resolve at configure time; this
         step is the belt-and-braces fallback for the cases where they do
         not (e.g. xeus-zmq's optional LibUV branch on a stripped vcpkg).
      3. Verifies stdlib source + precompiled artifacts exist:
         stdlib\prelude.eta and stdlib\prelude.etac.
      4. Verifies the resulting bin\ contains the full runtime DLL set
         plus the MSVC redistributable runtime (msvcp140 / vcruntime140 /
         vcruntime140_1).

    Exit codes:
        0  bundle looks good
        1  one or more required artefacts missing
        2  invalid arguments (e.g. prefix does not exist)

.PARAMETER Prefix
    The install prefix produced by `cmake --install`. Required.

.PARAMETER VcpkgDir
    Optional path to a vcpkg checkout (e.g. D:\a\_temp\vcpkg). Used to source
    OpenSSL DLLs from <VcpkgDir>\installed\x64-windows\bin when they are
    missing from the bundle.

.PARAMETER DepsDir
    Optional path to the CMake build/_deps directory used to source any DLLs
    produced by FetchContent (xeus, xeus-zmq, libzmq, libuv, openssl).

.PARAMETER NoCopy
    If supplied, only validate; do not attempt to copy missing DLLs in.

.EXAMPLE
    pwsh -File scripts/check-windows-bundle.ps1 `
        -Prefix C:\Users\lewis\develop\eta\dist\eta-v0.5.1-win-x64

.EXAMPLE
    pwsh -File scripts/check-windows-bundle.ps1 `
        -Prefix D:\a\eta\eta\dist\eta-v0.5.1-win-x64 `
        -VcpkgDir D:\a\_temp\vcpkg `
        -DepsDir D:\a\eta\eta\build\_deps
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Prefix,

    [string] $VcpkgDir,

    [string] $DepsDir,

    [switch] $NoCopy
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Prefix)) {
    Write-Error "Install prefix does not exist: $Prefix"
    exit 2
}

$binDir = Join-Path $Prefix 'bin'
if (-not (Test-Path $binDir)) {
    Write-Error "Bundle bin\ directory missing: $binDir"
    exit 1
}

Write-Host "=== Validating bundle: $Prefix ==="

# ── 1. Required executables ─────────────────────────────────────────────────
$requiredExes = @(
    'eta.exe', 'etac.exe', 'etai.exe', 'eta_test.exe', 'eta_repl.exe',
    'eta_lsp.exe', 'eta_dap.exe', 'eta_jupyter.exe'
)
$missingExes = @()
foreach ($exe in $requiredExes) {
    $p = Join-Path $binDir $exe
    if (-not (Test-Path $p)) { $missingExes += $exe }
}

$stdlibDir = Join-Path $Prefix 'stdlib'
$requiredStdlib = @('prelude.eta', 'prelude.etac')
$missingStdlib = @()
foreach ($artifact in $requiredStdlib) {
    $p = Join-Path $stdlibDir $artifact
    if (-not (Test-Path $p)) { $missingStdlib += "stdlib\$artifact" }
}

# ── 2. Hydrate runtime DLLs from vcpkg / _deps ──────────────────────────────
# CMake's in-tree install(FILES $<TARGET_FILE:...>) rules in
# eta/tools/jupyter/CMakeLists.txt only fire when the corresponding FetchContent or
# imported target is fully materialised at configure time. On CI runners that
# is not always the case (e.g. xeus-zmq's FetchContent build skips libuv when
# LibUV is not on the vcpkg manifest), so the bundle's bin\ ends up missing
# DLLs the kernel needs at runtime. Belt-and-braces: copy any matching DLL
# we can find under vcpkg's installed bin and under build/_deps before the
# validation pass runs.
function Add-CandidateDlls {
    param(
        [System.Collections.ArrayList] $Acc,
        [string] $Root,
        [string[]] $Filters,
        [switch] $Recurse
    )
    if (-not $Root) { return }
    if (-not (Test-Path $Root)) { return }
    foreach ($f in $Filters) {
        $items = if ($Recurse) {
            Get-ChildItem -Path $Root -Recurse -Filter $f -File -ErrorAction SilentlyContinue
        } else {
            Get-ChildItem -Path $Root -Filter $f -File -ErrorAction SilentlyContinue
        }
        foreach ($i in $items) { [void] $Acc.Add($i) }
    }
}

# Every DLL family the eta_jupyter kernel needs at runtime. Patterns are
# globs evaluated against vcpkg bin (flat) and against build/_deps (recursive).
$hydrationFilters = @(
    'xeus.dll', 'libxeus.dll',
    'xeus-zmq.dll', 'libxeus-zmq.dll',
    'libzmq*.dll', 'zmq*.dll',
    'uv.dll', 'libuv*.dll',
    'libcrypto*.dll', 'libssl*.dll',
    'libsodium*.dll'
)

if (-not $NoCopy) {
    $candidates = New-Object System.Collections.ArrayList
    if ($VcpkgDir) {
        $vcpkgBin = Join-Path $VcpkgDir 'installed\x64-windows\bin'
        Add-CandidateDlls -Acc $candidates -Root $vcpkgBin -Filters $hydrationFilters
    }
    Add-CandidateDlls -Acc $candidates -Root $DepsDir -Filters $hydrationFilters -Recurse

    # Prefer Release builds over Debug when both exist under build/_deps.
    $unique = $candidates |
        Sort-Object @{Expression = { if ($_.FullName -match '\\Debug\\') { 1 } else { 0 } }},
                    Name,
                    FullName -Unique
    foreach ($dll in $unique) {
        $dest = Join-Path $binDir $dll.Name
        if (-not (Test-Path $dest)) {
            Write-Host "Copying $($dll.FullName) -> $binDir"
            Copy-Item -Force $dll.FullName $binDir -ErrorAction SilentlyContinue
        }
    }
}

# ── 3. Required runtime DLL set ─────────────────────────────────────────────
function Test-AnyDll {
    param([string] $Dir, [string[]] $Patterns)
    foreach ($pat in $Patterns) {
        if ($pat -match '[*?]') {
            $hit = Get-ChildItem -Path $Dir -Filter $pat -File -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { return $true }
        } else {
            if (Test-Path (Join-Path $Dir $pat)) { return $true }
        }
    }
    return $false
}

$dllChecks = @(
    @{ Label = 'xeus'      ; Patterns = @('xeus.dll', 'libxeus.dll') }
    @{ Label = 'xeus-zmq'  ; Patterns = @('xeus-zmq.dll', 'libxeus-zmq.dll') }
    @{ Label = 'libzmq'    ; Patterns = @('libzmq*.dll', 'zmq*.dll') }
    @{ Label = 'libuv'     ; Patterns = @('uv.dll', 'libuv*.dll') }
    @{ Label = 'libcrypto' ; Patterns = @('libcrypto*.dll') }
)

$missingDlls = @()
foreach ($c in $dllChecks) {
    if (-not (Test-AnyDll -Dir $binDir -Patterns $c.Patterns)) {
        $missingDlls += "$($c.Label) ($($c.Patterns -join ' / '))"
    }
}

# ── 4. MSVC redistributable runtime ─────────────────────────────────────────
$msvcDlls = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$missingMsvc = @()
foreach ($d in $msvcDlls) {
    if (-not (Test-Path (Join-Path $binDir $d))) { $missingMsvc += $d }
}

# ── Report ─────────────────────────────────────────────────────────────────
$ok = $true
if ($missingExes.Count -gt 0) {
    $ok = $false
    Write-Host "[FAIL] Missing executables: $($missingExes -join ', ')"
}
if ($missingStdlib.Count -gt 0) {
    $ok = $false
    Write-Host "[FAIL] Missing stdlib artifacts: $($missingStdlib -join ', ')"
}
if ($missingDlls.Count -gt 0) {
    $ok = $false
    Write-Host "[FAIL] Missing eta_jupyter runtime DLLs: $($missingDlls -join ', ')"
}
if ($missingMsvc.Count -gt 0) {
    $ok = $false
    Write-Host "[FAIL] Missing MSVC runtime DLLs: $($missingMsvc -join ', ')"
}

if ($ok) {
    Write-Host "[OK] Bundle bin\ contains all required artefacts."
    exit 0
}

Write-Host ""
Write-Host "Current bin\ contents:"
Get-ChildItem -Path $binDir -File |
    Sort-Object Name |
    Select-Object Name, Length |
    Format-Table -AutoSize | Out-String -Width 200 | Write-Host

exit 1

