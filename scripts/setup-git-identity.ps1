# Pin this repo to the human owner. Run once after clone (or if an agent changed identity).
# CA-048: run from a UTF-8 console (chcp 65001) or PowerShell 7 — Windows
# PowerShell 5 with a legacy code page double-encodes 'Yüksel' to 'YÃ¼ksel'.
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$Name = 'Batuhan Yüksel'
$Email = 'batu3384@users.noreply.github.com'

git config --local user.name $Name
git config --local user.email $Email
git config --local i18n.commitEncoding utf-8
git config --local core.hooksPath .githooks

Write-Host "Git identity for this repo:"
git config --local --get user.name
git config --local --get user.email
Write-Host "Hooks: .githooks/pre-commit blocks bot/agent authors."
