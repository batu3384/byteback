import { join } from 'path'

export function nativeAddonCandidates(dirname: string, resourcesPath?: string): string[] {
  const rel = join(dirname, '../../native/build/Release/byteback_engine.node')
  const unpacked = resourcesPath
    ? join(resourcesPath, 'app.asar.unpacked', 'native', 'build', 'Release', 'byteback_engine.node')
    : ''
  return unpacked ? [rel, unpacked] : [rel]
}

// CA-010: absolute directory holding signatures-extended.json /
// signatures-supplement.json for the native carver. Dev resolves against the
// repo resources/ folder; packaged against the asar-unpacked copy.
export function signaturesDirCandidates(dirname: string, resourcesPath?: string): string[] {
  const dev = join(dirname, '../../resources')
  const unpacked = resourcesPath
    ? join(resourcesPath, 'app.asar.unpacked', 'resources')
    : ''
  return unpacked ? [unpacked, dev] : [dev]
}
