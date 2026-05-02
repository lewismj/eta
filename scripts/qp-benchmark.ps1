param(
    [string]$BuildDir = "C:\Users\lewis\develop\eta\out\msvc-release",
    [string]$Sizes = "8,16,24,32",
    [int]$Repeats = 25,
    [double]$Lambda = 2.0,
    [double]$Upper = 0.35,
    [switch]$Gate,
    [string]$OutputFile = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-BenchmarkExe {
    param([string]$Root)

    $candidates = @(
        (Join-Path $Root "eta/qa/bench/eta_qp_bench.exe"),
        (Join-Path $Root "eta/qa/bench/Release/eta_qp_bench.exe")
    )

    foreach ($path in $candidates) {
        if (Test-Path $path) {
            return $path
        }
    }

    throw "eta_qp_bench.exe not found under '$Root'. Build eta_all first."
}

$exe = Resolve-BenchmarkExe -Root $BuildDir

$inv = [System.Globalization.CultureInfo]::InvariantCulture
$args = @(
    "--sizes", $Sizes,
    "--repeats", $Repeats.ToString($inv),
    "--lambda", $Lambda.ToString("G17", $inv),
    "--upper", $Upper.ToString("G17", $inv)
)

if ($Gate) {
    $args += "--gate"
}

if ([string]::IsNullOrWhiteSpace($OutputFile)) {
    & $exe @args
    exit $LASTEXITCODE
}

& $exe @args | Tee-Object -FilePath $OutputFile
exit $LASTEXITCODE
