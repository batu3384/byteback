import React, { useState } from 'react'
import './ResultsView.css'

interface RecoveredFile {
  id: string
  name: string
  path: string
  sizeBytes: number
  confidence: number
  category: string
  source: string
}

const MOCK_FILES: RecoveredFile[] = [
  { id: '1', name: 'vacation_photo.jpg', path: '/Recovered/Images', sizeBytes: 2450000, confidence: 98, category: 'Image', source: 'ext4_inode' },
  { id: '2', name: 'financial_report_Q3.pdf', path: '/Recovered/Documents', sizeBytes: 1250000, confidence: 95, category: 'Document', source: 'hfs_catalog' },
  { id: '3', name: 'family_video.mp4', path: '/Recovered/Videos', sizeBytes: 250420000, confidence: 85, category: 'Video', source: 'exfat_dir' },
  { id: '4', name: 'project_backup.zip', path: '/Recovered/Archives', sizeBytes: 45000000, confidence: 100, category: 'Archive', source: 'ntfs_mft' },
  { id: '5', name: 'unknown_carved_1.png', path: '/Recovered/Raw', sizeBytes: 1024000, confidence: 60, category: 'Image', source: 'carver' },
  { id: '6', name: 'presentation_draft.pptx', path: '/Recovered/Documents', sizeBytes: 5400000, confidence: 92, category: 'Document', source: 'apfs_object' },
]

function ResultsView(): React.ReactElement {
  const [selectedFile, setSelectedFile] = useState<RecoveredFile | null>(null)
  const [activeCategory, setActiveCategory] = useState<string>('All')

  const categories = ['All', ...Array.from(new Set(MOCK_FILES.map(f => f.category)))]
  
  const filteredFiles = activeCategory === 'All' 
    ? MOCK_FILES 
    : MOCK_FILES.filter(f => f.category === activeCategory)

  const formatSize = (bytes: number) => {
    if (bytes < 1024) return bytes + ' B'
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB'
    if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(2) + ' MB'
    return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB'
  }

  const getConfidenceColor = (score: number) => {
    if (score >= 90) return 'var(--success)'
    if (score >= 70) return 'var(--warning)'
    return 'var(--danger)'
  }

  return (
    <div className="results-view">
      <div className="results-sidebar">
        <h3>Categories</h3>
        <ul className="category-list">
          {categories.map(cat => (
            <li 
              key={cat} 
              className={activeCategory === cat ? 'active' : ''}
              onClick={() => setActiveCategory(cat)}
            >
              {cat}
            </li>
          ))}
        </ul>
      </div>
      
      <div className="results-main">
        <div className="results-toolbar">
          <h3>Recovered Files ({filteredFiles.length})</h3>
          <button className="btn-primary">Export Selected</button>
        </div>
        
        <div className="files-grid">
          <div className="files-header">
            <span>Name</span>
            <span>Path</span>
            <span>Size</span>
            <span>Confidence</span>
          </div>
          <div className="files-body">
            {filteredFiles.map(file => (
              <div 
                key={file.id} 
                className={`file-row ${selectedFile?.id === file.id ? 'selected' : ''}`}
                onClick={() => setSelectedFile(file)}
              >
                <span className="file-name">{file.name}</span>
                <span className="file-path">{file.path}</span>
                <span className="file-size">{formatSize(file.sizeBytes)}</span>
                <span className="file-confidence">
                  <span 
                    className="confidence-dot" 
                    style={{ backgroundColor: getConfidenceColor(file.confidence) }}
                  />
                  {file.confidence}%
                </span>
              </div>
            ))}
          </div>
        </div>
      </div>

      <div className="results-preview">
        <h3>Preview</h3>
        {selectedFile ? (
          <div className="preview-content">
            <div className="preview-placeholder">
              <span>{selectedFile.category} Preview</span>
              <p className="preview-note">Preview engine loading...</p>
            </div>
            <div className="preview-metadata">
              <p><strong>Name:</strong> {selectedFile.name}</p>
              <p><strong>Size:</strong> {formatSize(selectedFile.sizeBytes)}</p>
              <p><strong>Source:</strong> <span className="mono">{selectedFile.source}</span></p>
              <p><strong>Integrity:</strong> {selectedFile.confidence}%</p>
            </div>
          </div>
        ) : (
          <div className="preview-empty">
            <p>Select a file to preview its contents and metadata.</p>
          </div>
        )}
      </div>
    </div>
  )
}

export default ResultsView
