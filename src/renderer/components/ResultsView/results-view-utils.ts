import type { FileRecord } from '../../../shared/ipc-contract'
import { t, getLang } from '../../i18n'

export type MappedFile = {
  id: number
  name: string
  rawPath: string
  rawStatus: number
  size: string
  path: string
  type: string
  status: string
  statusKey: string
  sourceLabel: string
  dateLabel: string
  qualityLabel: string
  confidence?: number
  confidenceTier: 'high' | 'mid' | 'low' | 'none'
}

export type TreeNode = {
  name: string
  path: string
  dirs: Map<string, TreeNode>
  files: MappedFile[]
}

export function qualityHint(raw?: FileRecord): string {
  if (!raw) return t('quality.none')
  const c = raw.confidence ?? 0
  if (raw.source === 'carver' || raw.source === 'carver_bgc') {
    if (c >= 85) return t('quality.carve.full')
    if (c >= 60) return t('quality.carve.suspect')
    return t('quality.carve.weak')
  }
  if (c > 0) {
    if (c >= 85) return t('quality.high')
    if (c >= 60) return t('quality.mid')
    return t('quality.low')
  }
  return t('quality.none')
}

/** Honest status: carve ≠ metadata-deleted. Key form lets callers style by
 *  classification instead of matching localized text. */
export function statusDisplayKey(status?: number, source?: string): string {
  if (source?.startsWith('carver')) return 'status.carved'
  if (status === 1) return 'status.allocated'
  return 'status.deleted'
}

export function statusDisplayLabel(status?: number, source?: string): string {
  return t(statusDisplayKey(status, source))
}

export function getExtension(filename: string): string {
  const parts = filename.split('.')
  return parts.length > 1 ? parts.pop()?.toLowerCase() || '' : ''
}

export function getFileType(ext: string): string {
  if (['jpg', 'png', 'gif', 'jpeg', 'bmp', 'webp', 'heic', 'heif', 'avif', 'tiff', 'svg'].includes(ext)) return 'img'
  if (['doc', 'docx', 'pdf', 'txt', 'xls', 'xlsx', 'ppt', 'pptx'].includes(ext)) return 'doc'
  if (['mp4', 'avi', 'mkv', 'mov', 'flv', 'wmv', 'm4v', 'webm'].includes(ext)) return 'video'
  if (['mp3', 'wav', 'flac', 'ogg', 'aac'].includes(ext)) return 'audio'
  if (['zip', 'rar', '7z', 'gz', 'tar', 'iso'].includes(ext)) return 'archive'
  return 'other'
}

/** Prefer DB category / extension over filename alone. */
export function resolveFileTypeChip(record: {
  name: string
  category?: string
  extension?: string
}): string {
  const cat = (record.category || '').toLowerCase()
  if (cat === 'image') return 'img'
  if (cat === 'document') return 'doc'
  if (cat === 'video') return 'video'
  if (cat === 'audio') return 'audio'
  if (cat === 'archive' || cat === 'container') return 'archive'
  const ext = (record.extension || getExtension(record.name)).replace(/^\./, '').toLowerCase()
  return getFileType(ext)
}

/** MFT timestamps; carve uses EXIF in modifiedAt when present; else honest placeholder. */
export function formatFsTimestamp(unixSec?: number, source?: string): string {
  const locale = getLang() === 'tr' ? 'tr-TR' : 'en-US'
  if (!unixSec || unixSec <= 0) {
    if (source?.startsWith('carver')) return t('ts.noFsDate')
    return '—'
  }
  const date = new Date(unixSec * 1000).toLocaleString(locale)
  if (source?.startsWith('carver')) return `EXIF · ${date}`
  return date
}

export function chipToCategory(chip: string): string {
  if (chip === 'img') return 'Image'
  if (chip === 'doc') return 'Document'
  if (chip === 'video') return 'Video'
  if (chip === 'audio') return 'Audio'
  if (chip === 'archive') return 'Archive'
  return ''
}

export type StatusChip = 'deleted' | 'allocated' | 'all' | 'carved'

/** CA-030: sortable column keys; values map to the native ORDER BY whitelist. */
export type SortField = 'confidence' | 'size' | 'name' | 'date' | 'id'
export type SortDir = 'asc' | 'desc'

export function sortKey(field: SortField, dir: SortDir): string {
  if (field === 'id') return ''
  return `${field}_${dir}`
}

/** Confidence chip tier for triage coloring. */
export function confidenceTier(c?: number): 'high' | 'mid' | 'low' | 'none' {
  if (typeof c !== 'number' || c <= 0) return 'none'
  if (c >= 80) return 'high'
  if (c >= 50) return 'mid'
  return 'low'
}

export function toSqlListFilter(
  statusChip: StatusChip,
  typeChip: string,
  query: string,
  showDuplicates: boolean,
  orderBy?: string,
  extra?: { sizeMin?: number; sizeMax?: number; dateFrom?: number; dateTo?: number },
): {
  status: number
  category: string
  query: string
  sourceLike: string
  sourceNotLike: string
  includeDuplicates: boolean
  includeDiscovery: boolean
  orderBy?: string
  sizeMin?: number
  sizeMax?: number
  dateFrom?: number
  dateTo?: number
} {
  const base = {
    category: chipToCategory(typeChip),
    query,
    sourceLike: '',
    sourceNotLike: '',
    includeDuplicates: showDuplicates,
    includeDiscovery: false,
    orderBy: orderBy || undefined,
    sizeMin: extra?.sizeMin,
    sizeMax: extra?.sizeMax,
    dateFrom: extra?.dateFrom,
    dateTo: extra?.dateTo,
  }
  if (statusChip === 'carved') return { ...base, status: -1, sourceLike: 'carver%' }
  // Metadata deleted only — carve lives under "Oyulmuş" (DiskDrill/Recuva style split).
  if (statusChip === 'deleted') return { ...base, status: 0, sourceNotLike: 'carver%' }
  if (statusChip === 'allocated') return { ...base, status: 1 }
  return { ...base, status: -1 }
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
    // Bare filename must stay a file at root — never a fake directory node.
    let dirParts: string[]
    if (parts.length === 0) dirParts = []
    else if (parts[parts.length - 1] === f.name) dirParts = parts.slice(0, -1)
    else if (parts.length === 1) dirParts = []
    else dirParts = parts
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
