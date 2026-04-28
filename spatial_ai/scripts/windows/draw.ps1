# Frame-by-frame draw: N drawing passes on a CE grid seeded from
# either a keyframe (inside a .spai) or an input image. Every pass
# produces a "video frame" PNG; the last frame is the output.
#
# Usage — seed from a trained keyframe:
#     PS> scripts\windows\draw.ps1 `
#           -Memory out\models\run.imem `
#           -Model  out\models\run.spai `
#           -SeedKf 0 -Frames 8
#
# Usage — seed from an image:
#     PS> scripts\windows\draw.ps1 `
#           -Memory out\models\run.imem `
#           -SeedImage assets\characters\char_01_ruby.png
#
# Output lands under out\draw\<Name>\ with:
#     frames\frame_000.png ... frame_NNN.png
#     final.png  <- the last frame, i.e. the result

param(
    [Parameter(Mandatory=$true)][string]$Memory,
    [string]$Model      = "",
    [int]$SeedKf        = -1,
    [string]$SeedImage  = "",
    [string]$Name       = "run",
    [string]$OutRoot    = "out",
    [int]$Frames        = 8,
    [int]$TopG          = 4,
    [double]$Penalty    = 0.5
)

$ErrorActionPreference = "Stop"

$repo    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$engine  = Join-Path $repo "spatial_ai"
$binDraw = Join-Path $engine "build\draw.exe"
if (-not (Test-Path $binDraw)) {
    $binDraw = Join-Path $engine "build\draw"
    if (-not (Test-Path $binDraw)) {
        throw "build/draw not found — run scripts\windows\build.ps1 first"
    }
}

$outDir = Join-Path $repo "$OutRoot\draw\$Name"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$args = @(
    "--memory", $Memory,
    "--out",    $outDir,
    "--frames", $Frames,
    "--top-g",  $TopG,
    "--penalty",$Penalty
)
if ($Model)     { $args += @("--model",      $Model)     }
if ($SeedKf -ge 0)  { $args += @("--seed-kf", $SeedKf)   }
if ($SeedImage) { $args += @("--seed-image", $SeedImage) }

Write-Host "[draw] $binDraw $($args -join ' ')" -ForegroundColor Cyan
& $binDraw @args
if ($LASTEXITCODE -ne 0) { throw "draw failed" }

$final = Join-Path $outDir "final.png"
Write-Host "" -ForegroundColor Green
Write-Host "[draw] done — final: $final" -ForegroundColor Green
