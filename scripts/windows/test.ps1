# Run every unit suite under spatial_ai/tests/ through mingw32-make.
#
# Usage:
#     PS> scripts\windows\test.ps1

param([string]$Make = "mingw32-make")

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$engine = Join-Path $repo "spatial_ai"

Push-Location $engine
try {
    Write-Host "[test] mingw32-make test" -ForegroundColor Cyan
    & $Make test
    if ($LASTEXITCODE -ne 0) { throw "make test failed" }
    Write-Host "[test] all suites green" -ForegroundColor Green
}
finally {
    Pop-Location
}
