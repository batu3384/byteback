# ponytail: release-dist refuses unsigned dist without -AllowUnsigned (exit 2).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Remove-Item Env:CSC_LINK -ErrorAction SilentlyContinue
Remove-Item Env:CSC_KEY_PASSWORD -ErrorAction SilentlyContinue
$p = Start-Process powershell.exe -ArgumentList @(
  '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
  (Join-Path $root 'scripts\release-dist.ps1'),
  '-SkipAudit'
) -WorkingDirectory $root -Wait -PassThru -NoNewWindow
if ($p.ExitCode -ne 2) {
  Write-Error "expected exit code 2 without CSC_LINK, got $($p.ExitCode)"
}
Write-Host 'release-dist.selfcheck: ok'
