import { describe, it, expect } from 'vitest'
import { nativeAddonCandidates } from './native-addon-path'
import { join } from 'path'

describe('nativeAddonCandidates', () => {
  it('includes the unpacked asar path for packaged builds', () => {
    const list = nativeAddonCandidates('/app/out/main', '/app/resources')
    expect(list[0].replace(/\\/g, '/')).toContain('native/build/Release/wolf_engine.node')
    expect(list[1]).toBe(
      join('/app/resources', 'app.asar.unpacked', 'native', 'build', 'Release', 'wolf_engine.node'),
    )
  })
})
