import { describe, it, expect } from 'vitest'
import { signaturesDirCandidates } from './native-addon-path'
import { join } from 'path'

// CA-010: the packaged app must prefer the asar-unpacked resources copy —
// std::ifstream in the native carver cannot read inside app.asar.
describe('signaturesDirCandidates', () => {
  it('prefers the unpacked asar resources dir when packaged', () => {
    const list = signaturesDirCandidates('/app/out/main', '/app/resources')
    expect(list[0]).toBe(join('/app/resources', 'app.asar.unpacked', 'resources'))
    expect(list[1].replace(/\\/g, '/')).toBe('/app/resources')
  })

  it('falls back to the dev repo resources dir', () => {
    const list = signaturesDirCandidates('/repo/out/main')
    expect(list[0].replace(/\\/g, '/')).toBe('/repo/resources')
  })
})
