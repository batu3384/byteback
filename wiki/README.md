# Wiki source (not published)

Markdown in this directory is the **source of truth** for the GitHub Wiki.
It is **not** copied to the wiki repo (`README.md` is excluded from sync).

## Publish

Requirements: `gh` authenticated, `git`, `rsync`, `bash` (Git Bash on Windows).

```bash
./scripts/sync-wiki.sh
```

Or from repo root on Windows (Git Bash):

```bash
bash scripts/sync-wiki.sh
```

## First-time setup

1. Enable Wiki on the GitHub repository (Settings → Features → Wikis).
2. Open the wiki in the browser and **create any page once** (e.g. empty Home) so GitHub initializes the wiki git repo.
3. Run `scripts/sync-wiki.sh`.

## What sync does

1. Reads `GITHUB_REPOSITORY` or `gh repo view --json nameWithOwner`.
2. Clones `https://github.com/<owner>/<repo>.wiki.git` to `.wiki-sync/` (token via `gh auth token`).
3. `rsync --delete` from `wiki/` → clone (excludes `.git`, `README.md`).
4. Commits and pushes to wiki `master` branch.

## Verify

Wiki URL: `https://github.com/<owner>/<repo>/wiki`

After sync: `https://github.com/batu3384/byteback/wiki/Home`
