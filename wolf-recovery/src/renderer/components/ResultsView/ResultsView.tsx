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
    if (['jpg', 'png', 'gif', 'jpeg', 'bmp'].includes(ext)) return 'img'
    if (['doc', 'docx', 'pdf', 'txt', 'xls', 'xlsx'].includes(ext)) return 'doc'
    if (['mp4', 'avi', 'mkv', 'mov'].includes(ext)) return 'video'
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
    size: formatSize(f.size),
    path: 'Recovered',
    status: 'Kurtarılabilir', // Mock status for now, since native doesn't provide it yet
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
          <button className="btn-secondary">Dışa Aktar</button>
          <button className="btn-primary">Seçilenleri Kurtar</button>
        </div>
      </div>

      <div className="results-content glass-panel">
        <div className="filters">
          <button className={`filter-btn ${filter === 'all' ? 'active' : ''}`} onClick={() => setFilter('all')}>Tümü</button>
          <button className={`filter-btn ${filter === 'img' ? 'active' : ''}`} onClick={() => setFilter('img')}>Resimler</button>
          <button className={`filter-btn ${filter === 'doc' ? 'active' : ''}`} onClick={() => setFilter('doc')}>Belgeler</button>
          <button className={`filter-btn ${filter === 'video' ? 'active' : ''}`} onClick={() => setFilter('video')}>Videolar</button>
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
