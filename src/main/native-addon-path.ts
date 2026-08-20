import { join } from 'path'

export function nativeAddonCandidates(dirname: string, resourcesPath?: string): string[] {
  const rel = join(dirname, '../../native/build/Release/byteback_engine.node')
  const unpacked = resourcesPath
    ? join(resourcesPath, 'app.asar.unpacked', 'native', 'build', 'Release', 'byteback_engine.node')
    : ''
  return unpacked ? [rel, unpacked] : [rel]
}
