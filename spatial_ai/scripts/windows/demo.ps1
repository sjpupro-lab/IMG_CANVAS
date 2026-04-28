# Run demo_pipeline on an image and drop plain + masked renders under
# out\demo\<Name>\. --adapt is on by default (per-image tier thresholds).
#
# Usage:
#     PS> scripts\windows\demo.ps1 -Image assets\main_hero.png
#     PS> scripts\windows\demo.ps1 -Image my.png -Name myrun -NoAdapt

param(
    [Parameter(Mandatory=$true)][string]$Image,
    [string]$Name    = "run",
    [string]$OutRoot = "out",
    [switch]$NoAdapt
)

$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$engine = Join-Path $repo "spatial_ai"
$bin    = Join-Path $engine "build\demo_pipeline.exe"
if (-not (Test-Path $bin)) {
    $bin = Join-Path $engine "build\demo_pipeline"
    if (-not (Test-Path $bin)) {
        throw "build/demo_pipeline not found — run scripts\windows\build.ps1 first"
    }
}

$outDir = Join-Path $repo "$OutRoot\demo"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$prefix = Join-Path $outDir $Name

$args = @()
if (-not $NoAdapt) { $args += "--adapt" }
$args += @($Image, $prefix)

Write-Host "[demo] $bin $($args -join ' ')" -ForegroundColor Cyan
& $bin @args
if ($LASTEXITCODE -ne 0) { throw "demo failed" }

Write-Host "" -ForegroundColor Green
Write-Host "[demo] outputs:" -ForegroundColor Green
Write-Host "  ${prefix}_plain.png"
Write-Host "  ${prefix}_masked.png"
