import { describe, it, expect } from 'vitest'
import { existsSync, mkdtempSync, realpathSync, rmSync } from 'fs'
import { join, parse } from 'path'
import { tmpdir } from 'os'
import {
  validateRecoverDestDir,
  isBlockedRecoverDest,
  strictResolveDest,
  ERR_DEST_NOT_ABSOLUTE,
  ERR_DEST_REQUIRED,
  ERR_DEST_BLOCKED,
  ERR_DEST_UNRESOLVED,
} from './recover-dest-validator'

// Policy is Windows-only by design (recovery targets are local drive paths);
// blocklist comparisons are pure string logic and OS-independent.
describe('isBlockedRecoverDest (pure policy)', () => {
  it('blocks a destination inside the system root, case-insensitive', () => {
    expect(isBlockedRecoverDest('C:\\Windows\\System32', { systemRoot: 'C:\\Windows' })).toBe('systemRoot')
    expect(isBlockedRecoverDest('c:\\wInDoWs\\sysWOW64', { systemRoot: 'C:\\Windows' })).toBe('systemRoot')
  })

  it('blocks the blocked root itself (not only subpaths)', () => {
    expect(isBlockedRecoverDest('C:\\Windows', { systemRoot: 'C:\\Windows' })).toBe('systemRoot')
  })

  it('does not block siblings that merely share a prefix (boundary check)', () => {
    expect(isBlockedRecoverDest('C:\\WindowsOld\\keep\\x', { systemRoot: 'C:\\Windows' })).toBeNull()
    expect(isBlockedRecoverDest('C:\\Program Files Rehab\\x', { programFiles: 'C:\\Program Files' })).toBeNull()
  })

  it('compares with normalized separators', () => {
    expect(isBlockedRecoverDest('C:/Windows/System32', { systemRoot: 'C:\\Windows' })).toBe('systemRoot')
  })

  it('blocks per-user and common Startup folders from env roots', () => {
    const appData = 'C:\\Users\\u\\AppData\\Roaming'
    const programData = 'C:\\ProgramData'
    expect(
      isBlockedRecoverDest(appData + '\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\evil.exe', { appData }),
    ).toBe('userStartup')
    expect(
      isBlockedRecoverDest(programData + '\\Microsoft\\Windows\\Start Menu\\Programs\\StartUp\\x', { programData }),
    ).toBe('commonStartup')
    expect(isBlockedRecoverDest(appData + '\\Documents\\safe', { appData })).toBeNull()
  })
})

describe('validateRecoverDestDir (structural + resolution, Windows host)', () => {
  it('rejects missing / non-string destinations', () => {
    expect(validateRecoverDestDir('')).toMatchObject({ ok: false, error: ERR_DEST_REQUIRED })
    expect(validateRecoverDestDir('   ')).toMatchObject({ ok: false, error: ERR_DEST_REQUIRED })
    expect(validateRecoverDestDir(null)).toMatchObject({ ok: false, error: ERR_DEST_REQUIRED })
    expect(validateRecoverDestDir(42)).toMatchObject({ ok: false, error: ERR_DEST_REQUIRED })
  })

  it('rejects relative and drive-relative paths instead of silently rebasing them', () => {
    expect(validateRecoverDestDir('foo\\bar')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('..\\..\\Windows\\Temp')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('C:relative\\path')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
  })

  it('rejects UNC shares, extended UNC and device paths', () => {
    expect(validateRecoverDestDir('\\\\server\\share\\dir')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('\\\\?\\UNC\\server\\share\\dir')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('//server/share/dir')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('\\\\.\\PhysicalDrive0')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('\\\\?\\Device\\HarddiskVolume2')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
  })

  it('rejects System32 via the stripped \\?\\ extended-length prefix', () => {
    expect(validateRecoverDestDir('\\\\?\\C:\\Windows\\System32')).toMatchObject({ ok: false, error: ERR_DEST_BLOCKED })
    expect(validateRecoverDestDir('\\\\?\\c:\\wIndows\\System32\\config')).toMatchObject({ ok: false, error: ERR_DEST_BLOCKED })
  })

  it('rejects mixed-case system roots without any prefix tricks', () => {
    expect(validateRecoverDestDir('c:\\wInDows\\Temp')).toMatchObject({ ok: false, error: ERR_DEST_BLOCKED })
  })

  it('rejects trailing dots/spaces that Win32 would normalize onto blocked roots', () => {
    // CreateDirectoryW("C:\Windows.") normalizes onto C:\Windows — the dotted
    // form must never reach native as an accepted dest.
    expect(validateRecoverDestDir('C:\\Windows.')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('C:\\Windows.\\Temp')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('C:\\Windows \\Temp')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
    expect(validateRecoverDestDir('C:\\Users\\Public\\.')).toMatchObject({ ok: false, error: ERR_DEST_NOT_ABSOLUTE })
  })

  it('rejects Program Files roots', () => {
    expect(validateRecoverDestDir('C:\\Program Files\\SomeApp')).toMatchObject({ ok: false, error: ERR_DEST_BLOCKED })
    expect(validateRecoverDestDir('C:\\Program Files (x86)\\SomeApp')).toMatchObject({ ok: false, error: ERR_DEST_BLOCKED })
  })

  it('collapses 8.3 short names onto the real path before the blocklist check', { skip: !existsSync('C:\\PROGRA~1') }, () => {
    expect(validateRecoverDestDir('C:\\PROGRA~1\\evil')).toMatchObject({ ok: false, error: ERR_DEST_BLOCKED })
  })

  it('rejects user Startup folder built from the real APPDATA root', () => {
    const appData = process.env.APPDATA
    if (!appData || !existsSync(appData)) return
    const startup = join(appData, 'Microsoft', 'Windows', 'Start Menu', 'Programs', 'Startup')
    expect(validateRecoverDestDir(startup)).toMatchObject({ ok: false, error: ERR_DEST_BLOCKED })
  })

  it('accepts a normal existing user folder and returns the resolved real path', () => {
    const dir = mkdtempSync(join(tmpdir(), 'byteback-recover-ok-'))
    try {
      const v = validateRecoverDestDir(dir)
      expect(v.ok).toBe(true)
      if (v.ok) {
        expect(v.destDir.toLowerCase()).toBe(realpathSync.native(dir).toLowerCase())
      }
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  it('accepts a not-yet-existing leaf by resolving the nearest existing ancestor', () => {
    const dir = mkdtempSync(join(tmpdir(), 'byteback-recover-new-'))
    try {
      const v = validateRecoverDestDir(join(dir, 'Recovered', 'deep'))
      expect(v.ok).toBe(true)
      if (v.ok) {
        expect(v.destDir.toLowerCase().startsWith(realpathSync.native(dir).toLowerCase())).toBe(true)
      }
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  it('accepts drive roots (user recovers to another drive)', () => {
    const root = parse(tmpdir()).root
    expect(validateRecoverDestDir(root)).toMatchObject({ ok: true })
  })

  it('accepts prefix-boundary siblings of blocked roots', () => {
    // C:\WindowsOld does not exist on most systems — walk-up resolves to C:\ and
    // the boundary check must still not treat it as inside C:\Windows.
    const v = validateRecoverDestDir('C:\\WindowsOld\\Recovered')
    expect(v.ok).toBe(true)
  })

  it('rejects a path on a non-existent volume instead of passing it to native', { skip: existsSync('Q:\\') }, () => {
    expect(validateRecoverDestDir('Q:\\Recovered')).toMatchObject({ ok: false, error: ERR_DEST_UNRESOLVED })
  })

  it('resolves through strictResolveDest helper directly', () => {
    expect(strictResolveDest('C:\\Windows\\System32')).not.toBeNull()
  })
})
