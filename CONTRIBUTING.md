# Contributing

## Git identity (required)

This repository must be committed under the project owner’s GitHub identity, not an
AI agent or bot account.

After cloning:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup-git-identity.ps1
```

Expected:

- **Name:** Batuhan Yüksel
- **Email:** `batu3384@users.noreply.github.com` (GitHub noreply — ties commits to [@batu3384](https://github.com/batu3384))

The `.githooks/pre-commit` hook rejects `Wolf Bot`, `Cursor Agent`, and
`bot@antigravity.dev` authors.

**Agents:** never run `git config user.name` or `user.email` to a bot identity in
this repo. Use the setup script above if identity is wrong.
