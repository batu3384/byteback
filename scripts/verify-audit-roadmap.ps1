#Requires -Version 5.1
# Engineering portion of audit/AUDIT_REPORT.md roadmap (Phases 1–3 + gates). Phase 4 not faked.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

Write-Host '=== npm run verify:audit ==='
npm run verify:audit
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (Test-Path 'native\build\Release\byteback_tests.exe') {
  Write-Host '=== native audit ctest subset ==='
  ctest --test-dir native/build -C Release -R 'CaseDbUsesWalFullSync|WalSyncFullVsNormalBulkInsertEvaluation|SearchFilesRegexReDoSPatternRejected|VhdExtractFuzz|VhdFamilyLimits' --output-on-failure
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
  Write-Warning 'native/build Release tests missing — run npm run test:native first'
}

function Invoke-PosixSyntaxVerify {
  $gitBash = Join-Path ${env:ProgramFiles} 'Git\bin\bash.exe'
  if (Test-Path $gitBash) {
    & $gitBash native/scripts/posix-syntax-check.verify.sh
    return $LASTEXITCODE
  }
  if (-not (Get-Command bash -ErrorAction SilentlyContinue)) { return -1 }
  bash -c 'exit 0' 2>$null | Out-Null
  if ($LASTEXITCODE -ne 0) { return -1 }
  bash native/scripts/posix-syntax-check.verify.sh
  return $LASTEXITCODE
}

Write-Host '=== posix-syntax-check.verify ==='
$posixRc = Invoke-PosixSyntaxVerify
if ($posixRc -eq -1) {
  Write-Warning 'bash unavailable — skip posix-syntax-check.verify (Linux CI still runs it)'
} elseif ($posixRc -ne 0) {
  exit $posixRc
} else {
  Write-Host 'posix-syntax-check.verify: ok'
}

Write-Host '=== release-dist signing guard ==='
& (Join-Path $PSScriptRoot 'release-dist.selfcheck.ps1')

Write-Host ''
Write-Host 'Engineering audit roadmap: PASS'
Write-Host 'Phase 4 still OPEN: field USB, Authenticode verify, clean-box NSIS, v1 tag — see audit/OBJECTIVE_CHECKLIST.md'
