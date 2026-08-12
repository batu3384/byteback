$worktrees = Get-ChildItem -Path 'C:\Users\Batuhan\.gemini\antigravity\brain\2eace21e-b297-4bd1-9805-fc4114077b66\.system_generated\worktrees\' -Directory
foreach ($wt in $worktrees) {
    Set-Location $wt.FullName
    git add -A
    git commit -m "Subagent work"
}
Set-Location 'C:\Users\Batuhan\Desktop\disk\wolf-recovery'
