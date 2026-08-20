# Pin this repo to the human owner. Run once after clone (or if an agent changed identity).
$ErrorActionPreference = 'Stop'
$Name = 'Batuhan Yüksel'
$Email = 'batu3384@users.noreply.github.com'

git config --local user.name $Name
git config --local user.email $Email
git config --local core.hooksPath .githooks

Write-Host "Git identity for this repo:"
git config --local --get user.name
git config --local --get user.email
Write-Host "Hooks: .githooks/pre-commit blocks bot/agent authors."
