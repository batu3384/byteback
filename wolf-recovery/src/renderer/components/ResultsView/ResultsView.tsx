import React, { useState } from 'react'
import './ResultsView.css'

interface ResultsViewProps {
  filesFound: any[]
}

function ResultsView({ filesFound }: ResultsViewProps): React.ReactElement {
  const [filter, setFilter] = useState('all')

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
    size: formatSize(f.sizeBytes || f.size),
    path: 'Recovered',
    status: f.status === 0 ? 'Kurtarılabilir' : 'Kısmen Bozuk',
    type: getFileType(getExtension(f.name))
  }))

  const filteredFiles = mappedFiles.filter(f => filter === 'all' || f.type === filter)

  return (
    <div className="results-view">
      <div className="results-header glass-panel">
        <div className="results-info">
          <h2>Kurtarma Sonuçları</h2>
          <p>Taranan disk üzerinde tespit edilen {filesFound.length} dosya</p>
        </div>
        <div className="results-actions">
          <button className="btn-secondary">Dışa Aktar (CSV/PDF)</button>
          <button className="btn-primary">Seçilenleri Kurtar</button>
        </div>
      </div>

      <div className="results-content glass-panel">
        <div className="filters">
          <button className={`filter-btn ${filter === 'all' ? 'active' : ''}`} onClick={() => setFilter('all')}>📁 Tümü</button>
          <button className={`filter-btn ${filter === 'img' ? 'active' : ''}`} onClick={() => setFilter('img')}>📸 Resimler</button>
          <button className={`filter-btn ${filter === 'doc' ? 'active' : ''}`} onClick={() => setFilter('doc')}>📄 Belgeler</button>
          <button className={`filter-btn ${filter === 'video' ? 'active' : ''}`} onClick={() => setFilter('video')}>🎬 Videolar</button>
          <button className={`filter-btn ${filter === 'audio' ? 'active' : ''}`} onClick={() => setFilter('audio')}>🎵 Ses</button>
          <button className={`filter-btn ${filter === 'archive' ? 'active' : ''}`} onClick={() => setFilter('archive')}>📦 Arşivler</button>
        </div>

        <table className="results-table">
          <thead>
            <tr>
              <th style={{ width: '40px' }}><input type="checkbox" /></th>
              <th>Dosya Adı</th>
              <th>Boyut</th>
              <th>Konum</th>
              <th>Durum</th>
            </tr>
          </thead>
          <tbody>
            {filteredFiles.length === 0 ? (
              <tr>
                <td colSpan={5} style={{ textAlign: 'center', padding: '2rem' }}>
                  Bu kategoride dosya bulunamadı.
                </td>
              </tr>
            ) : (
              filteredFiles.map((f) => (
                <tr key={f.id}>
                  <td><input type="checkbox" /></td>
                  <td className="file-name-cell">
                    <span className="file-icon">📄</span>
                    {f.name}
                  </td>
                  <td>{f.size}</td>
                  <td>{f.path}</td>
                  <td>
                    <span className={`status-badge ${f.status === 'Kurtarılabilir' ? 'success' : f.status === 'Kısmen Bozuk' ? 'warning' : 'danger'}`}>
                      {f.status}
                    </span>
                  </td>
                </tr>
              ))
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}

export default ResultsView
