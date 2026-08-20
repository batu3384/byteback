export function parseRecoverIds(
  scanId: unknown,
  fileId: unknown,
): { ok: true; scanId: number; fileId: number } | { ok: false; error: string } {
  const sid = typeof scanId === 'number' ? scanId : Number(scanId)
  const fid = typeof fileId === 'number' ? fileId : Number(fileId)
  if (!Number.isFinite(sid) || sid <= 0 || !Number.isFinite(fid) || fid <= 0) {
    return { ok: false, error: 'scan and file id required' }
  }
  return { ok: true, scanId: sid, fileId: fid }
}

export function parseRecoverIdList(
  scanId: unknown,
  fileIds: unknown,
): { ok: true; scanId: number; fileIds: number[] } | { ok: false; error: string } {
  const sid = typeof scanId === 'number' ? scanId : Number(scanId)
  if (!Number.isFinite(sid) || sid <= 0) {
    return { ok: false, error: 'scan and file id required' }
  }
  if (!Array.isArray(fileIds) || fileIds.length === 0) {
    return { ok: false, error: 'file ids required' }
  }
  const ids: number[] = []
  for (const raw of fileIds) {
    const fid = typeof raw === 'number' ? raw : Number(raw)
    if (!Number.isFinite(fid) || fid <= 0) {
      return { ok: false, error: 'scan and file id required' }
    }
    ids.push(fid)
  }
  return { ok: true, scanId: sid, fileIds: ids }
}
