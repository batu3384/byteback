#!/usr/bin/env bash
# Sync wiki/ markdown to GitHub Wiki (.wiki.git).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIKI_SRC="$ROOT/wiki"
SYNC_DIR="$ROOT/.wiki-sync"

if [[ ! -d "$WIKI_SRC" ]]; then
  echo "error: $WIKI_SRC not found" >&2
  exit 1
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "error: gh CLI required (https://cli.github.com/)" >&2
  exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
  echo "error: run 'gh auth login' first" >&2
  exit 1
fi

REPO="${GITHUB_REPOSITORY:-$(gh repo view --json nameWithOwner -q .nameWithOwner)}"
OWNER="${REPO%%/*}"
NAME="${REPO##*/}"
WIKI_REMOTE="https://github.com/${OWNER}/${NAME}.wiki.git"

TOKEN="$(gh auth token)"
AUTH_REMOTE="https://x-access-token:${TOKEN}@github.com/${OWNER}/${NAME}.wiki.git"

echo "Sync wiki → ${WIKI_REMOTE}"

if [[ -d "$SYNC_DIR/.git" ]]; then
  git -C "$SYNC_DIR" remote set-url origin "$AUTH_REMOTE"
  git -C "$SYNC_DIR" fetch origin
  git -C "$SYNC_DIR" checkout master 2>/dev/null || git -C "$SYNC_DIR" checkout -b master
  git -C "$SYNC_DIR" pull --rebase origin master || true
else
  rm -rf "$SYNC_DIR"
  git clone "$AUTH_REMOTE" "$SYNC_DIR"
  git -C "$SYNC_DIR" checkout master 2>/dev/null || git -C "$SYNC_DIR" checkout -b master
fi

if ! command -v rsync >/dev/null 2>&1; then
  echo "note: rsync not found — using cp fallback"
  find "$SYNC_DIR" -maxdepth 1 -type f -name '*.md' ! -name 'README.md' -delete 2>/dev/null || true
  shopt -s nullglob
  for f in "$WIKI_SRC"/*.md; do
    base="$(basename "$f")"
    [[ "$base" == "README.md" ]] && continue
    cp "$f" "$SYNC_DIR/$base"
  done
  shopt -u nullglob
else
  rsync -av --delete \
    --exclude='.git' \
    --exclude='README.md' \
    "$WIKI_SRC/" "$SYNC_DIR/"
fi

git -C "$SYNC_DIR" config user.name "$(git -C "$ROOT" config user.name 2>/dev/null || echo 'Wiki Sync')"
git -C "$SYNC_DIR" config user.email "$(git -C "$ROOT" config user.email 2>/dev/null || echo 'noreply@users.noreply.github.com')"

git -C "$SYNC_DIR" add -A
if git -C "$SYNC_DIR" diff --cached --quiet; then
  echo "Wiki already up to date."
  exit 0
fi

git -C "$SYNC_DIR" commit -m "wiki: sync from main repo $(date -u +%Y-%m-%dT%H:%MZ)"
git -C "$SYNC_DIR" push origin master

# Restore clean remote URL (no token in config)
git -C "$SYNC_DIR" remote set-url origin "$WIKI_REMOTE"

echo "Published: https://github.com/${OWNER}/${NAME}/wiki"
