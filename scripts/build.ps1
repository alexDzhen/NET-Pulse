$ErrorActionPreference = "Stop"

Set-Location (Join-Path $PSScriptRoot "..")

function Invoke-Step {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

Invoke-Step cmake @("-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release")
Invoke-Step cmake @("--build", "build", "--config", "Release", "--parallel")
Invoke-Step ctest @("--test-dir", "build", "-C", "Release", "--output-on-failure")

$binary = Get-ChildItem -Path build -Recurse -Filter netpulse.exe | Select-Object -First 1
Write-Host ""
Write-Host "Done. Run it with: $($binary.FullName) 127.0.0.1"
