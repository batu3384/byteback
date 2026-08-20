import type { FileRecord } from '../../../shared/ipc-contract'

export type MappedFile = {
  id: number
  name: string
  rawPath: string
  rawStatus: number
  size: string
  path: string
  type: string
  status: string
  sourceLabel: string
}

export type TreeNode = {
  name: string
  path: string
  dirs: Map<string, TreeNode>
  files: MappedFile[]
}

export function qualityHint(raw?: FileRecord): string {
  if (!raw) return '—'
  if (raw.source === 'carver' || raw.source === 'carver_bgc') {
    const c = raw.confidence ?? 0
    if (c >= 85) return 'Muhtemelen tam'
    if (c >= 60) return 'Şüpheli'
    return 'Zayıf'
  }
  return '—'
}

export function getExtension(filename: string): string {
  const parts = filename.split('.')
  return parts.length > 1 ? parts.pop()?.toLowerCase() || '' : ''
}

export function getFileType(ext: string): string {
  if (['jpg', 'png', 'gif', 'jpeg', 'bmp', 'webp', 'heic', 'tiff', 'svg'].includes(ext)) return 'img'
  if (['doc', 'docx', 'pdf', 'txt', 'xls', 'xlsx', 'ppt', 'pptx'].includes(ext)) return 'doc'
  if (['mp4', 'avi', 'mkv', 'mov', 'flv', 'wmv'].includes(ext)) return 'video'
  if (['mp3', 'wav', 'flac', 'ogg', 'aac'].includes(ext)) return 'audio'
  if (['zip', 'rar', '7z', 'gz', 'tar', 'iso'].includes(ext)) return 'archive'
  return 'other'
}

export function formatSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' B'
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB'
  if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(2) + ' MB'
  return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB'
}

export function buildTree(filteredFiles: MappedFile[]): TreeNode {
  const root: TreeNode = { name: '/', path: '', dirs: new Map(), files: [] }
  for (const f of filteredFiles) {
    const raw = (f.rawPath || f.name).replace(/\\/g, '/').replace(/^\/+/, '')
    const parts = raw.split('/').filter(Boolean)
    let node = root
    const dirParts = parts.length > 1 && parts[parts.length - 1] === f.name ? parts.slice(0, -1) : parts
    for (const part of dirParts) {
      if (!node.dirs.has(part)) {
        node.dirs.set(part, { name: part, path: (node.path ? node.path + '/' : '') + part, dirs: new Map(), files: [] })
      }
      node = node.dirs.get(part)!
    }
    node.files.push(f)
  }
  return root
}
