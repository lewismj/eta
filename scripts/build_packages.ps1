#Requires -Version 5.1
<#
.SYNOPSIS
    Rebuild non-stdlib packages and compile package Eta sources.

.DESCRIPTION
    Builds native sidecar packages under packages/ (excluding packages/stdlib),
    stages host artifacts into native/<arch>/libs/, updates host checksums in
    eta.toml, then runs `eta build --manifest-path` for every non-stdlib
    package manifest to produce package build artifacts (.etac).
#>
[CmdletBinding()]
param(
    [Parameter()]
    [string]$BuildRoot,

    [Parameter()]
    [string]$Config = "Release",

    [Parameter()]
    [string]$EtaExecutable,

    [Parameter()]
    [switch]$NoFetchUpstream,

    [Parameter()]
    [switch]$SkipNative,

    [Parameter()]
    [string]$CMakePrefixPath,

    [Parameter()]
    [string]$BoostDir,

    [Parameter()]
    [string]$BoostIncludeDir,

    [Parameter()]
    [string]$EtaCoreLibrary
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if (-not $BuildRoot) {
    $BuildRoot = Join-Path $ProjectRoot "out\package-build"
}
if (-not $EtaExecutable) {
    $EtaExecutable = Join-Path $ProjectRoot "out\msvc-release\eta\cli\eta.exe"
}

$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$EtaExecutable = [System.IO.Path]::GetFullPath($EtaExecutable)

if (-not (Test-Path -LiteralPath $EtaExecutable)) {
    throw "eta executable not found: $EtaExecutable"
}

function Get-HostTargetTriple {
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::Windows)) {
        if ($env:PROCESSOR_ARCHITECTURE -match "ARM64") {
            return "aarch64-pc-windows-msvc"
        }
        return "x86_64-pc-windows-msvc"
    }
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::Linux)) {
        $arch = (& uname -m).Trim()
        if ($arch -eq "x86_64") { return "x86_64-unknown-linux-gnu" }
        if ($arch -eq "aarch64" -or $arch -eq "arm64") { return "aarch64-unknown-linux-gnu" }
        throw "Unsupported Linux architecture: $arch"
    }
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::OSX)) {
        $arch = (& uname -m).Trim()
        if ($arch -eq "x86_64") { return "x86_64-apple-darwin" }
        if ($arch -eq "arm64" -or $arch -eq "aarch64") { return "aarch64-apple-darwin" }
        throw "Unsupported macOS architecture: $arch"
    }
    throw "Unsupported host OS for build_packages.ps1"
}

function Get-VcpkgHostTriplet {
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::Windows)) {
        if ($env:PROCESSOR_ARCHITECTURE -match "ARM64") {
            return "arm64-windows"
        }
        return "x64-windows"
    }
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::Linux)) {
        $arch = (& uname -m).Trim()
        if ($arch -eq "x86_64") { return "x64-linux" }
        if ($arch -eq "aarch64" -or $arch -eq "arm64") { return "arm64-linux" }
        throw "Unsupported Linux architecture for vcpkg triplet: $arch"
    }
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::OSX)) {
        $arch = (& uname -m).Trim()
        if ($arch -eq "x86_64") { return "x64-osx" }
        if ($arch -eq "aarch64" -or $arch -eq "arm64") { return "arm64-osx" }
        throw "Unsupported macOS architecture for vcpkg triplet: $arch"
    }
    throw "Unsupported host OS for vcpkg triplet resolution"
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)] [string]$CachePath,
        [Parameter(Mandatory = $true)] [string]$Key
    )
    if (-not (Test-Path -LiteralPath $CachePath)) {
        return ""
    }
    $line = Get-Content -LiteralPath $CachePath |
        Where-Object { $_ -match "^${Key}:[^=]*=(.*)$" } |
        Select-Object -First 1
    if (-not $line) {
        return ""
    }
    if ($line -match "^${Key}:[^=]*=(.*)$") {
        return $Matches[1]
    }
    return ""
}

function Resolve-EtaCoreLibrary {
    param(
        [Parameter(Mandatory = $true)] [string]$ProjectRoot,
        [Parameter(Mandatory = $true)] [string]$EtaExecutable,
        [Parameter(Mandatory = $true)] [string]$Config,
        [Parameter()] [string]$ExplicitPath = ""
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $explicitFull = [System.IO.Path]::GetFullPath($ExplicitPath)
        if (-not (Test-Path -LiteralPath $explicitFull)) {
            throw "ETA_CORE_LIBRARY does not exist: $explicitFull"
        }
        return $explicitFull
    }

    $etaExeDir = Split-Path -Parent $EtaExecutable
    $isWindows = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Windows)
    $fileName = if ($isWindows) { "eta_core.lib" } else { "libeta_core.a" }

    $candidates = @()
    if ($isWindows) {
        $candidates += (Join-Path $etaExeDir "..\..\core\$Config\$fileName")
        $candidates += (Join-Path $etaExeDir "..\..\core\$fileName")
        $candidates += (Join-Path $etaExeDir "..\core\$Config\$fileName")
        $candidates += (Join-Path $etaExeDir "..\core\$fileName")
        $candidates += (Join-Path $ProjectRoot "build\eta\core\$Config\$fileName")
        $candidates += (Join-Path $ProjectRoot "build\eta\core\$fileName")
    } else {
        $candidates += (Join-Path $etaExeDir "../core/$fileName")
        $candidates += (Join-Path $etaExeDir "../../core/$fileName")
        $candidates += (Join-Path $etaExeDir "../core/$Config/$fileName")
        $candidates += (Join-Path $etaExeDir "../../core/$Config/$fileName")
        $candidates += (Join-Path $ProjectRoot "build/eta/core/$fileName")
        $candidates += (Join-Path $ProjectRoot "build/eta/core/$Config/$fileName")
    }

    foreach ($candidate in $candidates) {
        $full = [System.IO.Path]::GetFullPath($candidate)
        if (Test-Path -LiteralPath $full) {
            return $full
        }
    }

    throw "Could not resolve eta_core library from eta executable path. " +
        "Pass -EtaCoreLibrary explicitly."
}

$cachePath = Join-Path $ProjectRoot "out\msvc-release\CMakeCache.txt"
if (-not $CMakePrefixPath) {
    $CMakePrefixPath = Get-CMakeCacheValue -CachePath $cachePath -Key "CMAKE_PREFIX_PATH"
}
if (-not $BoostDir) {
    $BoostDir = Get-CMakeCacheValue -CachePath $cachePath -Key "Boost_DIR"
}
if (-not $BoostIncludeDir) {
    $BoostIncludeDir = Get-CMakeCacheValue -CachePath $cachePath -Key "Boost_INCLUDE_DIR"
}
$vcpkgRoot = ""
if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT) -and (Test-Path -LiteralPath $env:VCPKG_ROOT)) {
    $vcpkgRoot = [System.IO.Path]::GetFullPath($env:VCPKG_ROOT)
} elseif (-not [string]::IsNullOrWhiteSpace($env:VCPKG_DIR) -and (Test-Path -LiteralPath $env:VCPKG_DIR)) {
    $vcpkgRoot = [System.IO.Path]::GetFullPath($env:VCPKG_DIR)
}
if ($vcpkgRoot) {
    $vcpkgTriplet = Get-VcpkgHostTriplet
    $vcpkgInstalled = Join-Path $vcpkgRoot "installed\$vcpkgTriplet"
    if (Test-Path -LiteralPath $vcpkgInstalled) {
        if (-not $CMakePrefixPath) {
            $CMakePrefixPath = $vcpkgInstalled
        }
        if (-not $BoostDir) {
            $boostShare = Join-Path $vcpkgInstalled "share\boost"
            $boostHeadersShare = Join-Path $vcpkgInstalled "share\boost-headers"
            if (Test-Path -LiteralPath $boostShare) {
                $BoostDir = $boostShare
            } elseif (Test-Path -LiteralPath $boostHeadersShare) {
                $BoostDir = $boostHeadersShare
            }
        }
        if (-not $BoostIncludeDir) {
            $candidate = Join-Path $vcpkgInstalled "include"
            if (Test-Path -LiteralPath $candidate) {
                $BoostIncludeDir = $candidate
            }
        }
    }
}

function Resolve-SidecarBinary {
    param(
        [Parameter(Mandatory = $true)] [string]$BuildDir,
        [Parameter(Mandatory = $true)] [string]$Config,
        [Parameter(Mandatory = $true)] [string]$BaseName,
        [Parameter()] [string]$PackageRoot = ""
    )

    $isWindows = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Windows)
    $isLinux = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Linux)

    if ($isWindows) {
        $fileName = "${BaseName}.dll"
    } elseif ($isLinux) {
        $fileName = "lib${BaseName}.so"
    } else {
        $fileName = "lib${BaseName}.dylib"
    }

    $candidates = @(
        (Join-Path $BuildDir (Join-Path $Config $fileName)),
        (Join-Path $BuildDir $fileName)
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }

    $match = Get-ChildItem -Path $BuildDir -Recurse -File -Filter $fileName |
        Select-Object -First 1
    if ($null -ne $match) {
        return $match.FullName
    }

    if (-not [string]::IsNullOrWhiteSpace($PackageRoot)) {
        $packageLibRoot = Join-Path $PackageRoot "libs"
        if (Test-Path -LiteralPath $packageLibRoot) {
            $packageMatch = Get-ChildItem -Path $packageLibRoot -Recurse -File -Filter $fileName |
                Select-Object -First 1
            if ($null -ne $packageMatch) {
                return $packageMatch.FullName
            }
        }
    }

    throw "Could not locate sidecar binary $fileName under $BuildDir"
}

function Invoke-NativeBuild {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$PackageRoot,
        [Parameter(Mandatory = $true)] [string]$BuildDir,
        [Parameter(Mandatory = $true)] [string]$TargetName,
        [Parameter(Mandatory = $true)] [string]$BaseName,
        [Parameter(Mandatory = $true)] [string]$StageScript,
        [Parameter(Mandatory = $true)] [string[]]$ConfigureArgs
    )

    Write-Host "> Building native package: $Name"
    $fetchValue = if ($NoFetchUpstream) { "OFF" } else { "ON" }
    $fullConfigureArgs = @("-S", $PackageRoot, "-B", $BuildDir) + $ConfigureArgs
    $fullConfigureArgs = $fullConfigureArgs |
        ForEach-Object { $_.Replace("{FETCH_UPSTREAM}", $fetchValue) }

    & cmake @fullConfigureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for $Name"
    }

    & cmake --build $BuildDir --config $Config --target $TargetName
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed for $Name"
    }

    $sidecarBinary = Resolve-SidecarBinary `
        -BuildDir $BuildDir `
        -Config $Config `
        -BaseName $BaseName `
        -PackageRoot $PackageRoot
    $hostTriple = Get-HostTargetTriple

    & cmake `
        "-DPACKAGE_ROOT=$PackageRoot" `
        "-DSIDECAR_BINARY=$sidecarBinary" `
        "-DHOST_TARGET_TRIPLE=$hostTriple" `
        -P $StageScript
    if ($LASTEXITCODE -ne 0) {
        throw "Artifact staging failed for $Name"
    }
}

if (-not $SkipNative) {
    $DuckdbRoot = Join-Path $ProjectRoot "packages\db\native\duckdb"
    $LightgbmRoot = Join-Path $ProjectRoot "packages\ml\native\lightgbm"
    $HttpRoot = Join-Path $ProjectRoot "packages\net\native\http"
    $EtaCoreLibrary = Resolve-EtaCoreLibrary `
        -ProjectRoot $ProjectRoot `
        -EtaExecutable $EtaExecutable `
        -Config $Config `
        -ExplicitPath $EtaCoreLibrary
    Write-Host "  using ETA_CORE_LIBRARY=$EtaCoreLibrary"

    $HttpConfigureArgs = @(
        "-DETA_HTTP_FETCH_UPSTREAM={FETCH_UPSTREAM}",
        "-DETA_HTTP_ENABLE_TESTS=OFF",
        "-DETA_CORE_LIBRARY=$EtaCoreLibrary"
    )
    if ($CMakePrefixPath) {
        $HttpConfigureArgs += "-DCMAKE_PREFIX_PATH=$CMakePrefixPath"
    }
    if ($BoostDir) {
        $HttpConfigureArgs += "-DBoost_DIR=$BoostDir"
    }
    if ($BoostIncludeDir) {
        $HttpConfigureArgs += "-DBoost_INCLUDE_DIR=$BoostIncludeDir"
    }

    Invoke-NativeBuild `
        -Name "http" `
        -PackageRoot $HttpRoot `
        -BuildDir (Join-Path $BuildRoot "http") `
        -TargetName "eta_http" `
        -BaseName "eta_http" `
        -StageScript (Join-Path $HttpRoot "cmake\StageHttpSidecar.cmake") `
        -ConfigureArgs $HttpConfigureArgs

    $DuckdbConfigureArgs = @(
        "-DETA_DUCKDB_FETCH_UPSTREAM={FETCH_UPSTREAM}",
        "-DETA_DUCKDB_ENABLE_TESTS=OFF",
        "-DETA_CORE_LIBRARY=$EtaCoreLibrary"
    )
    if ($CMakePrefixPath) {
        $DuckdbConfigureArgs += "-DCMAKE_PREFIX_PATH=$CMakePrefixPath"
    }
    if ($BoostDir) {
        $DuckdbConfigureArgs += "-DBoost_DIR=$BoostDir"
    }
    if ($BoostIncludeDir) {
        $DuckdbConfigureArgs += "-DBoost_INCLUDE_DIR=$BoostIncludeDir"
    }

    Invoke-NativeBuild `
        -Name "duckdb" `
        -PackageRoot $DuckdbRoot `
        -BuildDir (Join-Path $BuildRoot "duckdb") `
        -TargetName "eta_duckdb" `
        -BaseName "eta_duckdb" `
        -StageScript (Join-Path $DuckdbRoot "cmake\StageDuckDBSidecar.cmake") `
        -ConfigureArgs $DuckdbConfigureArgs

    $LightgbmConfigureArgs = @(
        "-DETA_LIGHTGBM_FETCH_UPSTREAM={FETCH_UPSTREAM}",
        "-DETA_LIGHTGBM_ENABLE_TESTS=OFF",
        "-DETA_CORE_LIBRARY=$EtaCoreLibrary"
    )
    if ($CMakePrefixPath) {
        $LightgbmConfigureArgs += "-DCMAKE_PREFIX_PATH=$CMakePrefixPath"
    }
    if ($BoostDir) {
        $LightgbmConfigureArgs += "-DBoost_DIR=$BoostDir"
    }
    if ($BoostIncludeDir) {
        $LightgbmConfigureArgs += "-DBoost_INCLUDE_DIR=$BoostIncludeDir"
    }

    Invoke-NativeBuild `
        -Name "lightgbm" `
        -PackageRoot $LightgbmRoot `
        -BuildDir (Join-Path $BuildRoot "lightgbm") `
        -TargetName "eta_lightgbm" `
        -BaseName "eta_lightgbm" `
        -StageScript (Join-Path $LightgbmRoot "cmake\StageLightGBMSidecar.cmake") `
        -ConfigureArgs $LightgbmConfigureArgs
}

Write-Host "> Building Eta package artifacts (.etac) for non-stdlib packages"
$PackageManifests = Get-ChildItem -Path (Join-Path $ProjectRoot "packages") -Recurse -File -Filter "eta.toml" |
    Where-Object { $_.FullName -notmatch '[\\/]+packages[\\/]+stdlib[\\/]' } |
    Sort-Object FullName

$StdlibModulePath = Join-Path $ProjectRoot "stdlib"
$EffectiveModulePath = ""
if (Test-Path -LiteralPath $StdlibModulePath) {
    $EffectiveModulePath = $StdlibModulePath
} elseif (-not [string]::IsNullOrWhiteSpace($env:ETA_MODULE_PATH)) {
    $EffectiveModulePath = $env:ETA_MODULE_PATH
}

$PreviousModulePath = $env:ETA_MODULE_PATH
if (-not [string]::IsNullOrWhiteSpace($EffectiveModulePath)) {
    Write-Host "  using ETA_MODULE_PATH=$EffectiveModulePath"
    $env:ETA_MODULE_PATH = $EffectiveModulePath
}

try {
foreach ($manifest in $PackageManifests) {
    Write-Host "  - $($manifest.FullName)"
    & $EtaExecutable build --manifest-path $manifest.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "eta build failed for manifest: $($manifest.FullName)"
    }
}
}
finally {
    if ($null -eq $PreviousModulePath) {
        Remove-Item Env:ETA_MODULE_PATH -ErrorAction SilentlyContinue
    } else {
        $env:ETA_MODULE_PATH = $PreviousModulePath
    }
}

Write-Host ""
Write-Host "[OK] Package rebuild complete." -ForegroundColor Green
