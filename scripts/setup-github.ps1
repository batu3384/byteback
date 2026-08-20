# Creates github.com/<you>/byteback and pushes main. Requires: gh auth login
param(
  [string]$RepoName = 'byteback',
  [ValidateSet('private', 'public')]
  [string]$Visibility = 'private'
)

$ErrorActionPreference = 'Stop'
$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path', 'User')

Push-Location (Split-Path $PSScriptRoot -Parent)
try {
  gh auth status | Out-Null
  $origin = git remote 2>$null | Select-String -Pattern '^origin$' -Quiet
  if ($origin) {
    Write-Host 'Remote origin already set:' (git remote get-url origin)
    git push -u origin main
    exit 0
  }
  gh repo create $RepoName --$Visibility --source=. --remote=origin --push --description 'Byteback — professional Windows forensic data recovery (Electron + C++)'
  Write-Host "Done: https://github.com/$(gh api user -q .login)/$RepoName"
} finally {
  Pop-Location
}
