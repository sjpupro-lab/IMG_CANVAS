# Run bimodal training on a TSV manifest and drop the .spai / .imem
# under out\models\.
#
# Usage:
#     PS> scripts\windows\train.ps1 -Manifest spatial_ai\data\characters_manifest.tsv
#     PS> scripts\windows\train.ps1 -Manifest my.tsv -Name myrun
#     PS> scripts\windows\train.ps1 -Manifest my.tsv -Resume

param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [string]$Name     = "run",
    [string]$OutRoot  = "out",
    [switch]$Resume
)

$ErrorActionPreference = "Stop"

$repo     = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$engine   = Join-Path $repo "spatial_ai"
$binTrain = Join-Path $engine "build\train.exe"
if (-not (Test-Path $binTrain)) {
    $binTrain = Join-Path $engine "build\train"
    if (-not (Test-Path $binTrain)) {
        throw "build/train not found — run scripts\windows\build.ps1 first"
    }
}

$outDir   = Join-Path $repo "$OutRoot\models"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$spai = Join-Path $outDir "$Name.spai"
$imem = Join-Path $outDir "$Name.imem"

$args = @("--model", $spai, "--memory", $imem)
if ($Resume) { $args += "--resume" }
$args += $Manifest

Write-Host "[train] $binTrain $($args -join ' ')" -ForegroundColor Cyan
& $binTrain @args
if ($LASTEXITCODE -ne 0) { throw "train failed" }

Write-Host "" -ForegroundColor Green
Write-Host "[train] done" -ForegroundColor Green
Write-Host "  model:  $spai"
Write-Host "  memory: $imem"
