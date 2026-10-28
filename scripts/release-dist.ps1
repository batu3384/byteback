#Requires -Version 5.1
<#
.SYNOPSIS
  Release build gate: verify:audit, full build, NSIS dist. Signs only when CSC_* is set.
.NOTES
  Does not fake Authenticode PASS. Without CSC_LINK, electron-builder skips signing.
#>
param(
  [switch] $SkipAudit,
  [switch] $AllowUnsigned
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

if (-not $SkipAudit) {
  Write-Host 'Running npm run verify:audit...'
  npm run verify:audit
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$signed = [bool]$env:CSC_LINK
if (-not $signed) {
  Write-Warning 'CSC_LINK is not set — installer will be UNSIGNED (see docs/release-signing.env.example).'
  if (-not $AllowUnsigned) {
    Write-Host 'Re-run with -AllowUnsigned to proceed, or set CSC_LINK / CSC_KEY_PASSWORD first.'
    exit 2
  }
}

Write-Host 'Building native + Electron...'
npm run build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host 'Packaging NSIS...'
npm run dist
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$setup = Get-ChildItem -Path release -Filter 'Byteback Setup *.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $setup) {
  Write-Error 'NSIS setup exe not found under release/'
}

Write-Host "Output: $($setup.FullName)"
if ($signed) {
  Write-Host 'Verify signature: signtool verify /pa "' + $setup.FullName + '"'
} else {
  Write-Host 'Checklist: Authenticode remains [ ] until signtool verify passes on a signed build.'
}
