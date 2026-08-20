# Run once after closing Cursor/Electron — renames workspace folder and removes stale junk.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/finalize-byteback.ps1

$ErrorActionPreference = 'Stop'
$src = Join-Path $env:USERPROFILE 'Desktop\disk'
$dst = Join-Path $env:USERPROFILE 'Desktop\byteback'

if (-not (Test-Path $src)) {
  Write-Host "Source folder not found: $src (already renamed?)"
  exit 0
}

if (Test-Path $dst) {
  Write-Host "Target already exists: $dst — remove duplicate manually if needed."
  exit 1
}

$junk = Join-Path $src 'byteback'
if (Test-Path $junk) {
  Write-Host "Removing stale $junk ..."
  Remove-Item -Recurse -Force $junk -ErrorAction SilentlyContinue
}

Write-Host "Renaming $src -> $dst"
Rename-Item -LiteralPath $src -NewName 'byteback'
Write-Host 'Done. Reopen Cursor from Desktop\byteback'
