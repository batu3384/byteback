import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { describe, expect, it } from 'vitest'

const repoRoot = path.resolve(import.meta.dirname, '../..')

describe('audit Phase 4 status gate (operator)', () => {
  it.skipIf(process.platform !== 'win32')(
    'npm run phase4:status exits 1 while checklist rows are open',
    () => {
      const r = spawnSync('npm run phase4:status', {
        cwd: repoRoot,
        shell: true,
        encoding: 'utf8',
        timeout: 120_000,
      })
      expect(r.status).toBe(1)
      const out = `${r.stdout ?? ''}${r.stderr ?? ''}`
      expect(out).toContain('Row A')
      expect(out).toContain('PHASE4_OPERATOR_RUNBOOK')
      expect(out).toContain('release:dist:unsigned')
      const openRows = out.split('\n').filter((line) => /^\s*- \[ \]/.test(line))
      expect(openRows.length).toBe(6)
      const labInstaller = path.join(repoRoot, 'release', 'Byteback Setup 0.1.0.exe')
      if (fs.existsSync(labInstaller)) {
        expect(out).toContain('Lab NSIS')
        expect(out).toMatch(/SHA-256: [0-9A-F]{64}/)
      }
    },
    120_000,
  )
})
