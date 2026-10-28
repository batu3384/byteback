#Requires -Version 5.1
# Reports Phase 4 audit goal blockers (exit 1 until honest PASS). Does not mutate logs.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$checklist = Join-Path $root 'docs\release-checklist.md'
$usbLog = Join-Path $root 'audit\FIELD_USB_LOG.md'
$vmLog = Join-Path $root 'audit\PHASE4_VM_SMOKE_LOG.md'
$signLog = Join-Path $root 'audit\PHASE4_SIGN_LOG.md'

$patterns = @(
    @{ Key = 'Row A —'; Usb = 'A' },
    @{ Key = 'Row B —'; Usb = 'B' },
    @{ Key = 'Row C —'; Usb = 'C' },
    @{ Key = 'Authenticode code sign'; Usb = $null; Sign = $true },
    @{ Key = 'NSIS installer smoke'; Usb = $null; Vm = $true },
    @{ Key = 'CHANGELOG for the tagged version'; Usb = $null; Changelog = $true }
)

function UsbRowPass([string]$logPath, [string]$letter) {
    if (-not (Test-Path $logPath)) { return $false }
    $t = Get-Content $logPath -Raw -Encoding UTF8
    return $t -match ("## Row $letter[\s\S]*?PASS / FAIL:\s*\r?\n\s*-\s*PASS")
}

function VmSmokePass([string]$logPath) {
    if (-not (Test-Path $logPath)) { return $false }
    $t = Get-Content $logPath -Raw -Encoding UTF8
    return $t -match '## VM run[\s\S]*?PASS / FAIL:\s*\r?\n\s*-\s*PASS'
}

function SignLogPass([string]$logPath) {
    if (-not (Test-Path $logPath)) { return $false }
    $t = Get-Content $logPath -Raw -Encoding UTF8
    return $t -match '## Verification[\s\S]*?PASS / FAIL:\s*\r?\n\s*-\s*PASS'
}

function ChangelogTaggedPass([string]$path) {
    if (-not (Test-Path $path)) { return $false }
    $t = Get-Content $path -Raw -Encoding UTF8
    return $t -match '## \[(?!Unreleased)[^\]]+\]'
}

$changelog = Join-Path $root 'CHANGELOG.md'
$open = @()
foreach ($entry in $patterns) {
    $line = Select-String -Path $checklist -Pattern $entry.Key -SimpleMatch | Select-Object -First 1
    if (-not $line) { continue }
    $text = $line.Line.Trim()
    if ($text -match '- \[ \]') {
        $open += $text
        continue
    }
    if ($text -notmatch '- \[x\]') { continue }
    if ($entry.Usb -and -not (UsbRowPass $usbLog $entry.Usb)) {
        $open += "DISHONEST: checklist [x] for Row $($entry.Usb) but FIELD_USB_LOG has no PASS"
    }
    if ($entry.Vm -and -not (VmSmokePass $vmLog)) {
        $open += 'DISHONEST: checklist [x] for NSIS but PHASE4_VM_SMOKE_LOG has no PASS'
    }
    if ($entry.Sign -and -not (SignLogPass $signLog)) {
        $open += 'DISHONEST: checklist [x] for Authenticode but PHASE4_SIGN_LOG has no PASS'
    }
    if ($entry.Changelog -and -not (ChangelogTaggedPass $changelog)) {
        $open += 'DISHONEST: checklist [x] for CHANGELOG but CHANGELOG.md has no released version section'
    }
}

Write-Host 'Audit thread goal — Phase 4'
Write-Host "Checklist: $checklist"
Write-Host "USB log:   $usbLog (must contain PASS lines before rows are checked [x])"
Write-Host "VM log:    $vmLog (NSIS clean-box PASS/FAIL before NSIS row [x])"
Write-Host "Sign log:  $signLog (signtool PASS before Authenticode row [x])"
Write-Host ''

if ($open.Count -eq 0) {
    Write-Host 'Release checklist Phase 4 rows are [x] with log parity (where applicable).'
    Write-Host 'Operator: confirm signtool verify output for Authenticode before UpdateGoal complete.'
    exit 0
}

Write-Host 'OPEN (thread goal incomplete):'
$open | ForEach-Object { Write-Host "  $_" }
Write-Host ''
$installer = Join-Path $root 'release\Byteback Setup 0.1.0.exe'
if (Test-Path $installer) {
    $hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
    Write-Host "Lab NSIS (unsigned OK for VM smoke): $installer"
    Write-Host "SHA-256: $hash"
} else {
    Write-Host 'Lab NSIS missing — run: npm run release:dist:unsigned'
}
Write-Host ''
Write-Host 'Runbook: audit/PHASE4_OPERATOR_RUNBOOK.md'
Write-Host 'Commands: npm run field:usb:preflight | npm run release:dist (signed) | npm run release:dist:unsigned (lab NSIS, not Authenticode PASS)'
exit 1
