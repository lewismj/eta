#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDir = "C:\Users\lewis\develop\eta\out\msvc-release",
    [string]$CMakeExe = "C:\Program Files\JetBrains\CLion 2026.1\bin\cmake\win\x64\bin\cmake.exe",
    [string]$BuildTarget = "eta_all",
    [int]$Jobs = 14,
    [ValidateSet("none", "dap", "all")]
    [string]$CppTests = "dap",
    [switch]$SkipVsxTests,
    [switch]$SkipVsix,
    [switch]$NpmCi
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$VsCodeDir = Join-Path $RepoRoot "editors\vscode"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Resolve-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at '$vswhere'. Install Visual Studio Build Tools."
    }
    $installPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1).Trim()
    if (-not $installPath) {
        throw "No Visual Studio installation with C++ tools found."
    }
    $devCmd = Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $devCmd)) {
        throw "VsDevCmd.bat not found at '$devCmd'."
    }
    return @{
        InstallPath = $installPath
        DevCmd = $devCmd
    }
}

function Enter-VsDevShell {
    param([string]$VsDevCmdPath)
    $envDump = & cmd.exe /d /s /c "`"$VsDevCmdPath`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize Visual Studio Developer Shell."
    }
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
}

function Resolve-CMakeExe {
    param([string]$ConfiguredPath)
    if (Test-Path $ConfiguredPath) {
        return (Resolve-Path $ConfiguredPath).Path
    }
    $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmakeCmd) {
        throw "CMake executable not found. Either install cmake on PATH or pass -CMakeExe."
    }
    return $cmakeCmd.Source
}

function Resolve-EtaCoreTestBinary {
    param([string]$RootBuildDir)
    $candidates = @(
        (Join-Path $RootBuildDir "eta\qa\test\eta_core_test.exe"),
        (Join-Path $RootBuildDir "eta\qa\test\Release\eta_core_test.exe"),
        (Join-Path $RootBuildDir "eta_core_test.exe"),
        (Join-Path $RootBuildDir "Release\eta_core_test.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }
    $found = Get-ChildItem -Path $RootBuildDir -Recurse -File -Filter "eta_core_test.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($found) {
        return $found.FullName
    }
    return $null
}

function Invoke-Checked {
    param(
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $RepoRoot
    )
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            $argText = ($Arguments -join " ")
            throw "Command failed: $FilePath $argText"
        }
    } finally {
        Pop-Location
    }
}

$vs = Resolve-VsDevCmd
$resolvedCMake = Resolve-CMakeExe -ConfiguredPath $CMakeExe

Write-Host "Repo root : $RepoRoot"
Write-Host "VS install: $($vs.InstallPath)"
Write-Host "Build dir : $BuildDir"
Write-Host "CMake     : $resolvedCMake"
Write-Host "Target    : $BuildTarget"
Write-Host "Jobs      : $Jobs"
Write-Host "CPP tests : $CppTests"

Write-Step "Entering latest Visual Studio Developer Shell"
Enter-VsDevShell -VsDevCmdPath $vs.DevCmd

Write-Step "Building $BuildTarget"
Invoke-Checked -FilePath $resolvedCMake -Arguments @(
    "--build", $BuildDir,
    "--target", $BuildTarget,
    "-j", "$Jobs"
)

if ($CppTests -eq "dap") {
    Write-Step "Running DAP-focused C++ tests"
    $etaCoreTest = Resolve-EtaCoreTestBinary -RootBuildDir $BuildDir
    if ($etaCoreTest) {
        Invoke-Checked -FilePath $etaCoreTest -Arguments @("--run_test=dap_*/*")
    } else {
        Write-Host "eta_core_test.exe not found under '$BuildDir'. Falling back to ctest." -ForegroundColor Yellow
        Invoke-Checked -FilePath "ctest" -Arguments @(
            "--test-dir", $BuildDir,
            "-C", "Release",
            "--output-on-failure",
            "-R", "eta_core_test"
        )
    }
} elseif ($CppTests -eq "all") {
    Write-Step "Running full C++ test suite"
    Invoke-Checked -FilePath "ctest" -Arguments @(
        "--test-dir", $BuildDir,
        "-C", "Release",
        "--output-on-failure"
    )
}

Write-Step "Preparing VS Code extension dependencies"
if ($NpmCi) {
    Invoke-Checked -FilePath "npm" -Arguments @("ci") -WorkingDirectory $VsCodeDir
} else {
    if (-not (Test-Path (Join-Path $VsCodeDir "node_modules"))) {
        Invoke-Checked -FilePath "npm" -Arguments @("ci") -WorkingDirectory $VsCodeDir
    }
}

if (-not $SkipVsxTests) {
    Write-Step "Running VS Code extension tests"
    Invoke-Checked -FilePath "npm" -Arguments @("run", "compile-tests") -WorkingDirectory $VsCodeDir
    Invoke-Checked -FilePath "npm" -Arguments @("test") -WorkingDirectory $VsCodeDir
}

if (-not $SkipVsix) {
    Write-Step "Packaging VSIX"
    Invoke-Checked -FilePath "npm" -Arguments @("run", "package") -WorkingDirectory $VsCodeDir
    $vsix = Get-ChildItem -Path $VsCodeDir -File -Filter "eta-scheme-lang-*.vsix" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($vsix) {
        Write-Host "VSIX: $($vsix.FullName)" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
