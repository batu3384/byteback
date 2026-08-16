import React, { useState } from 'react'
import './ResultsView.css'
import { File, FileImage, FileText, FileVideo, FileAudio, FileArchive, Download, ShieldCheck, Folder, FolderOpen, ListTree, List } from 'lucide-react'

interface ResultsViewProps {
  filesFound: any[]
  driveIndex: number | null
  scanId?: number
}

function ResultsView({ filesFound, driveIndex, scanId }: ResultsViewProps): React.ReactElement {
  const [filter, setFilter] = useState('all')
  const [selectedFiles, setSelectedFiles] = useState<Set<number>>(new Set())
  const [isRecovering, setIsRecovering] = useState(false)
  const [viewMode, setViewMode] = useState<'tree' | 'flat'>('tree')
  const [expandedDirs, setExpandedDirs] = useState<Set<string>>(new Set())

  const handleRecover = async () => {
    if (selectedFiles.size === 0) return
    if (driveIndex === null) {
      alert('Kurtarma için bir sürücü seçili değil. Önce bir disk taraması başlatın.')
      return
    }
    if (!window.api?.recoverFile) {
      alert('Kurtarma API\'si kullanılamıyor.')
      return
    }

    setIsRecovering(true)

    // Use the real native directory picker instead of a hardcoded path.
    let destDir = await window.api.pickDirectory()
    if (!destDir) {
      // User cancelled the dialog.
      setIsRecovering(false)
      return
    }

    let successCount = 0
    let failedCount = 0

    for (const id of selectedFiles) {
      const fileToRecover = filesFound.find(f => f.id === id) || filesFound[id]
      if (fileToRecover) {
        try {
          const res = await window.api.recoverFile(driveIndex, fileToRecover, destDir, scanId)
          if (res.success) successCount++
          else failedCount++
        } catch {
          failedCount++
        }
      }
    }

    setIsRecovering(false)
    alert(`Kurtarma Tamamlandı!\nBaşarılı: ${successCount}\nBaşarısız: ${failedCount}\nHedef: ${destDir}`)
  }

  const toggleSelection = (id: number) => {
    const newSel = new Set(selectedFiles)
    if (newSel.has(id)) newSel.delete(id)
    else newSel.add(id)
    setSelectedFiles(newSel)
  }

  // Export the filtered results as CSV. UTF-8 BOM keeps Excel happy with
  // Turkish filenames; fields are escaped per RFC 4180.
  const exportCsv = () => {
    const rows = filteredFiles.map((f) => {
      const raw = filesFound.find((x) => x.id === f.id) ?? {}
      return {
        name: f.name,
        sizeBytes: raw.sizeBytes ?? raw.size ?? '',
        category: raw.category ?? '',
        confidence: raw.confidence ?? '',
        status: raw.status,
        path: raw.path ?? '',
        startSector: raw.startSector ?? '',
        createdAt: raw.createdAt ? new Date(raw.createdAt * 1000).toISOString() : '',
        modifiedAt: raw.modifiedAt ? new Date(raw.modifiedAt * 1000).toISOString() : '',
      }
    })
    const esc = (v: unknown) => {
      const s = String(v ?? '')
      return /[",\n;]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s
    }
    const header = ['name', 'sizeBytes', 'category', 'confidence', 'status', 'path', 'startSector', 'createdAt', 'modifiedAt']
    const csv = [header.join(';'), ...rows.map((r) => header.map((h) => esc((r as any)[h])).join(';'))].join('\r\n')
    const blob = new Blob(['\uFEFF' + csv], { type: 'text/csv;charset=utf-8' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `wolf-recovery-sonuclar-${new Date().toISOString().slice(0, 10)}.csv`
    a.click()
    URL.revokeObjectURL(url)
  }

  const toggleAll = (e: React.ChangeEvent<HTMLInputElement>) => {
    if (e.target.checked) {
      setSelectedFiles(new Set(filteredFiles.map(f => f.id)))
    } else {
      setSelectedFiles(new Set())
    }
  }

  const getExtension = (filename: string) => {
    const parts = filename.split('.')
    return parts.length > 1 ? parts.pop()?.toLowerCase() || '' : ''
  }

  const getFileType = (ext: string) => {
    if (['jpg', 'png', 'gif', 'jpeg', 'bmp', 'webp', 'heic', 'tiff', 'svg'].includes(ext)) return 'img'
    if (['doc', 'docx', 'pdf', 'txt', 'xls', 'xlsx', 'ppt', 'pptx'].includes(ext)) return 'doc'
    if (['mp4', 'avi', 'mkv', 'mov', 'flv', 'wmv'].includes(ext)) return 'video'
    if (['mp3', 'wav', 'flac', 'ogg', 'aac'].includes(ext)) return 'audio'
    if (['zip', 'rar', '7z', 'gz', 'tar', 'iso'].includes(ext)) return 'archive'
    return 'other'
  }

  const formatSize = (bytes: number) => {
    if (bytes < 1024) return bytes + ' B'
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB'
    if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(2) + ' MB'
    return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB'
  }

  const mappedFiles = filesFound.map((f, i) => ({
    id: i,
    name: f.name,
    rawPath: typeof f.path === 'string' ? f.path : '',
    size: formatSize(f.sizeBytes || f.size),
    path: 'Kurtarılanlar',
    status: f.status === 0 ? 'Kurtarılabilir' : 'Kısmen Bozuk',
    type: getFileType(getExtension(f.name))
  }))

  const filteredFiles = mappedFiles.filter(f => filter === 'all' || f.type === filter)

  // ---- Directory tree ----
  // Build a nested tree from the reconstructed paths (NTFS results carry
  // real paths from the INDX/MFT pass; carved files fall under "Kurtarılanlar").
  interface TreeNode {
    name: string
    path: string
    dirs: Map<string, TreeNode>
    files: typeof filteredFiles
  }
  const buildTree = (): TreeNode => {
    const root: TreeNode = { name: '/', path: '', dirs: new Map(), files: [] }
    for (const f of filteredFiles) {
      const raw = (f.rawPath || '/Kurtarılanlar/' + f.name).replace(/\\/g, '/').replace(/^\/+/, '')
      const parts = raw.split('/').filter(Boolean)
      // The path usually ends with the file name; use it as the leaf when it matches.
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
  const treeRoot = buildTree()

  const renderTreeNode = (node: TreeNode, depth: number): React.ReactNode[] => {
    const out: React.ReactNode[] = []
    const sortedDirs = Array.from(node.dirs.values()).sort((a, b) => a.name.localeCompare(b.name, 'tr'))
    for (const dir of sortedDirs) {
      const isOpen = expandedDirs.has(dir.path)
      const childCount = dir.files.length + dir.dirs.size
      out.push(
        <div
          key={'d:' + dir.path}
          onClick={() => {
            const next = new Set(expandedDirs)
            if (next.has(dir.path)) next.delete(dir.path)
            else next.add(dir.path)
            setExpandedDirs(next)
          }}
          style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '6px 12px', cursor: 'pointer', marginLeft: depth * 16, borderRadius: '4px' }}
          onMouseEnter={(e) => (e.currentTarget.style.background = 'rgba(255,255,255,0.03)')}
          onMouseLeave={(e) => (e.currentTarget.style.background = 'transparent')}
        >
          {isOpen ? <FolderOpen size={16} color="var(--accent-blue)" /> : <Folder size={16} color="var(--accent-blue)" />}
          <span style={{ fontWeight: 500 }}>{dir.name}</span>
          <span style={{ fontSize: '0.75rem', color: 'var(--text-muted)' }}>{childCount} öğe</span>
        </div>
      )
      if (isOpen) out.push(...renderTreeNode(dir, depth + 1))
    }
    for (const f of node.files) {
      out.push(
        <div
          key={'f:' + f.id}
          onClick={() => toggleSelection(f.id)}
          style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '6px 12px', marginLeft: (depth + 1) * 16, cursor: 'pointer', borderRadius: '4px', background: selectedFiles.has(f.id) ? 'rgba(59, 130, 246, 0.1)' : 'transparent' }}
        >
          <input type="checkbox" checked={selectedFiles.has(f.id)} onChange={() => toggleSelection(f.id)} style={{ width: 14, height: 14 }} />
          {getIconForType(f.type)}
          <span style={{ fontFamily: 'monospace', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{f.name}</span>
          <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: '0.8rem', flexShrink: 0 }}>{f.size}</span>
        </div>
      )
    }
    return out
  }

  const getIconForType = (type: string) => {
    switch (type) {
      case 'img': return <FileImage size={18} color="var(--accent-blue)" />
      case 'doc': return <FileText size={18} color="var(--success-green)" />
      case 'video': return <FileVideo size={18} color="var(--alert-red)" />
      case 'audio': return <FileAudio size={18} color="var(--warning-yellow)" />
      case 'archive': return <FileArchive size={18} color="#b700ff" />
      default: return <File size={18} color="var(--text-muted)" />
    }
  }

  return (
    <div className="results-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%' }}>
      <div className="results-header glass-panel" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '24px' }}>
        <div className="results-info">
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Kurtarma Sonuçları</h2>
          <p style={{ color: 'var(--text-muted)' }}>Taranan disk üzerinde tespit edilen {filesFound.length} dosya</p>
        </div>
        <div className="results-actions" style={{ display: 'flex', gap: '12px' }}>
          <button className="btn-secondary" style={{ display: 'flex', gap: '8px' }} onClick={exportCsv} disabled={filteredFiles.length === 0}>
            <Download size={16} /> Dışa Aktar (CSV)
          </button>
          <button 
            className="btn-primary" 
            style={{ display: 'flex', gap: '8px', opacity: selectedFiles.size === 0 ? 0.5 : 1, cursor: selectedFiles.size === 0 ? 'not-allowed' : 'pointer' }}
            onClick={handleRecover}
            disabled={selectedFiles.size === 0 || isRecovering}
          >
            <ShieldCheck size={16} /> 
            {isRecovering ? 'Kurtarılıyor...' : `Seçilenleri Kurtar (${selectedFiles.size})`}
          </button>
        </div>
      </div>

      <div className="results-content glass-panel" style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
        <div className="filters" style={{ display: 'flex', gap: '8px', padding: '16px 24px', borderBottom: '1px solid var(--panel-border)', alignItems: 'center', flexWrap: 'wrap' }}>
          <button
            className="btn-secondary"
            title={viewMode === 'tree' ? 'Düz liste görünümüne geç' : 'Dizin ağacı görünümüne geç'}
            onClick={() => setViewMode(viewMode === 'tree' ? 'flat' : 'tree')}
            style={{ padding: '6px 12px', display: 'flex', gap: '6px', alignItems: 'center' }}
          >
            {viewMode === 'tree' ? <List size={16} /> : <ListTree size={16} />}
            {viewMode === 'tree' ? 'Liste' : 'Ağaç'}
          </button>
          <button className={`btn-secondary ${filter === 'all' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'all' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('all')}>Tümü</button>
          <button className={`btn-secondary ${filter === 'img' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'img' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('img')}>Resimler</button>
          <button className={`btn-secondary ${filter === 'doc' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'doc' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('doc')}>Belgeler</button>
          <button className={`btn-secondary ${filter === 'video' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'video' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('video')}>Videolar</button>
          <button className={`btn-secondary ${filter === 'audio' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'audio' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('audio')}>Ses</button>
          <button className={`btn-secondary ${filter === 'archive' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'archive' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('archive')}>Arşivler</button>
        </div>

        <div style={{ flex: 1, overflowY: 'auto', padding: '0 24px' }} className={viewMode === 'tree' ? 'tree-container' : ''}>
          {viewMode === 'tree' ? (
          <div style={{ padding: '12px 0', display: 'flex', flexDirection: 'column', gap: '2px' }}>
            {filteredFiles.length === 0 ? (
              <div style={{ textAlign: 'center', padding: '3rem', color: 'var(--text-muted)' }}>Bu kategoride dosya bulunamadı.</div>
            ) : renderTreeNode(treeRoot, 0)}
          </div>
          ) : (
          <table className="results-table" style={{ width: '100%', borderCollapse: 'collapse', textAlign: 'left' }}>
            <thead style={{ position: 'sticky', top: 0, background: 'var(--bg-surface)', zIndex: 1 }}>
              <tr>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)', width: '40px' }}>
                  <input type="checkbox" checked={selectedFiles.size === filteredFiles.length && filteredFiles.length > 0} onChange={toggleAll} />
                </th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Dosya Adı</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Boyut</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Konum</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Durum</th>
              </tr>
            </thead>
            <tbody>
              {filteredFiles.length === 0 ? (
                <tr>
                  <td colSpan={5} style={{ textAlign: 'center', padding: '3rem', color: 'var(--text-muted)' }}>
                    Bu kategoride dosya bulunamadı.
                  </td>
                </tr>
              ) : (
                filteredFiles.map((f) => (
                  <tr key={f.id} style={{ borderBottom: '1px solid rgba(255,255,255,0.02)', background: selectedFiles.has(f.id) ? 'rgba(59, 130, 246, 0.1)' : 'transparent' }}>
                    <td style={{ padding: '12px' }}>
                      <input type="checkbox" checked={selectedFiles.has(f.id)} onChange={() => toggleSelection(f.id)} />
                    </td>
                    <td className="file-name-cell" style={{ padding: '12px', display: 'flex', alignItems: 'center', gap: '12px', fontFamily: 'monospace' }}>
                      {getIconForType(f.type)}
                      {f.name}
                    </td>
                    <td style={{ padding: '12px', color: 'var(--text-muted)' }}>{f.size}</td>
                    <td style={{ padding: '12px', color: 'var(--text-muted)' }}>{f.path}</td>
                    <td style={{ padding: '12px' }}>
                      <span style={{ 
                        padding: '4px 8px', borderRadius: '4px', fontSize: '0.8rem',
                        background: f.status === 'Kurtarılabilir' ? 'rgba(16, 185, 129, 0.1)' : 'rgba(245, 158, 11, 0.1)',
                        color: f.status === 'Kurtarılabilir' ? 'var(--success-green)' : 'var(--warning-yellow)',
                        border: `1px solid ${f.status === 'Kurtarılabilir' ? 'var(--success-green)' : 'var(--warning-yellow)'}`
                      }}>
                        {f.status}
                      </span>
                    </td>
                  </tr>
                ))
              )}
            </tbody>
          </table>
          )}
        </div>
      </div>
    </div>
  )
}

export default ResultsView
