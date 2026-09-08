import { describe, it, expect } from 'vitest'
import { redactPaths } from './redact-paths'

const paths = { userData: 'C:\\Users\\me\\AppData\\Roaming\\byteback', cwd: 'C:\\work\\byteback' }

describe('redactPaths', () => {
  it('replaces the userData prefix with <userData>', () => {
    expect(redactPaths('db at C:\\Users\\me\\AppData\\Roaming\\byteback\\byteback.db', paths)).toBe(
      'db at <userData>\\byteback.db',
    )
  })

  it('replaces the cwd prefix with <cwd>', () => {
    expect(redactPaths('at C:\\work\\byteback\\out\\main\\main.js:12', paths)).toBe('at <cwd>\\out\\main\\main.js:12')
  })

  it('keeps the stack shape and unrelated paths untouched', () => {
    const stack = 'Error: boom\n    at foo (C:\\work\\byteback\\out\\main\\main.js:9:11)\n    at bar (C:\\Windows\\System32\\x.js:1:1)'
    const redacted = redactPaths(stack, paths)
    expect(redacted).toContain('Error: boom')
    expect(redacted).toContain('at bar (C:\\Windows\\System32\\x.js:1:1)')
    expect(redacted).toContain('at foo (<cwd>\\out\\main\\main.js:9:11)')
    expect(redacted).not.toContain('C:\\work\\byteback')
  })

  it('redacts forward-slash and file:/// URI variants', () => {
    expect(redactPaths('C:/Users/me/AppData/Roaming/byteback/session.log', paths)).toBe('<userData>/session.log')
    expect(redactPaths('file:///C:/Users/me/AppData/Roaming/byteback/preload.js', paths)).toBe('file:///<userData>/preload.js')
  })

  it('is a no-op when nothing matches', () => {
    expect(redactPaths('nothing here', paths)).toBe('nothing here')
  })
})
