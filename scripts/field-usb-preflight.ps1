#Requires -Version 5.1
# Operator preflight — prints field USB checklist; does not mark PASS.
$root = Split-Path $PSScriptRoot -Parent
Write-Host 'Byteback field USB gate (docs/release-checklist.md rows A/B/C)'
Write-Host "Log template: $root\audit\FIELD_USB_LOG.md"
Write-Host 'Rows stay [ ] until MD5 evidence is recorded in the log.'
Write-Host ''
Get-Content (Join-Path $root 'docs\release-checklist.md') -Encoding UTF8 | Select-String -Pattern 'Row [ABC]|Field USB' -Context 0,0
