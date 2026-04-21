# Build the engine + every tool under spatial_ai/.
#
# Requires MSYS2 with mingw-w64 GCC on PATH (mingw32-make + gcc).
# Run from the repo root:
#     PS> scripts\windows\build.ps1
#
# Pass -Clean to nuke build/ first.

param(
    [switch]$Clean,
    [string]$Make = "mingw32-make"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$engine = Join-Path $repo "spatial_ai"

Push-Location $engine
try {
    if ($Clean) {
        Write-Host "[build] cleaning spatial_ai/build/" -ForegroundColor Cyan
        & $Make clean
    }

    Write-Host "[build] compiling engine + tools" -ForegroundColor Cyan
    & $Make all
    if ($LASTEXITCODE -ne 0) { throw "make all failed" }

    # Build the CLI tools explicitly so the binaries land in build/.
    foreach ($tool in @("demo_pipeline", "train", "draw", "stream_train", "chat", "gen_delta_tables")) {
        Write-Host "[build] build/$tool" -ForegroundColor DarkCyan
        & $Make "build/$tool"
        if ($LASTEXITCODE -ne 0) { throw "make build/$tool failed" }
    }

    Write-Host "[build] OK — binaries in $engine\build\" -ForegroundColor Green
}
finally {
    Pop-Location
}
