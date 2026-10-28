#Requires -Version 5.1
# Thread goal closure probe: engineering PASS + Phase 4 checklist closed (exit 0 only when both).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

Write-Host '=== Engineering (Phases 1–3) ==='
npm run verify:audit:full
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Goal closure: BLOCKED (engineering gate failed)'
    exit $LASTEXITCODE
}

Write-Host ''
Write-Host '=== Phase 4 (operator) ==='
npm run phase4:status
$phase4 = $LASTEXITCODE
if ($phase4 -ne 0) {
    Write-Host 'Goal closure: BLOCKED (Phase 4 open — see audit/PHASE4_OPERATOR_RUNBOOK.md)'
    exit $phase4
}

Write-Host ''
Write-Host 'Goal closure: all automated gates PASS — operator must still confirm signtool/VM/USB evidence in repo before UpdateGoal complete.'
