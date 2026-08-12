import React, { useState, useEffect } from 'react'
import './HexEditor.css'

interface HexEditorProps {
  driveIndex?: number | null
  sectorSize?: number
}

function HexEditor({ driveIndex, sectorSize = 512 }: HexEditorProps): React.ReactElement {
  const [sector, setSector] = useState(0)
  const [data, setData] = useState<number[]>([])
  const [loading, setLoading] = useState(false)

  const fetchSector = async (secIndex: number) => {
    if (driveIndex === undefined || driveIndex === null) return
    setLoading(true)
    try {
      const offset = secIndex * sectorSize
      if (window.api && window.api.readHexData) {
        const result = await window.api.readHexData(driveIndex, offset, sectorSize)
        if (result && Array.isArray(result)) {
          setData(result)
        } else {
          setData(new Array(sectorSize).fill(0))
        }
      }
    } catch (err) {
      console.error(err)
      setData(new Array(sectorSize).fill(0))
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    if (driveIndex !== undefined && driveIndex !== null) {
      fetchSector(sector)
    }
  }, [driveIndex, sector])

  // Shannon Entropy Calculation
  const calculateEntropy = (buffer: number[]) => {
    if (buffer.length === 0) return 0;
    const freq: Record<number, number> = {};
    for (let b of buffer) {
      freq[b] = (freq[b] || 0) + 1;
    }
    let entropy = 0;
    for (let key in freq) {
      const p = freq[key] / buffer.length;
      entropy -= p * Math.log2(p);
    }
    return entropy;
  };

  const currentEntropy = calculateEntropy(data);
  const entropyRatio = (currentEntropy / 8) * 100;
  let entropyClass = 'entropy-low';
  if (currentEntropy > 4.5) entropyClass = 'entropy-mid';
  if (currentEntropy > 7.0) entropyClass = 'entropy-high';

  const toHex = (num: number, padding: number = 2) => num.toString(16).toUpperCase().padStart(padding, '0')
  const toAscii = (num: number) => (num >= 32 && num <= 126 ? String.fromCharCode(num) : '.')

  if (driveIndex === undefined || driveIndex === null) {
    return (
      <div className="hex-editor empty">
        <div className="icon-large">💻</div>
        <h2>Hex İnceleyici</h2>
        <p>Sektör düzeyinde (Raw) veri analizi yapmak için Ana Ekran'dan bir sürücü seçin.</p>
      </div>
    )
  }

  return (
    <div className="hex-editor">
      <div className="hex-toolbar glass-panel">
        <div className="toolbar-info">
          <h3>Fiziksel Sürücü {driveIndex}</h3>
          <span className="badge">Read-Only</span>
        </div>
        <div className="sector-navigation">
          <button className="btn-secondary" onClick={() => setSector(s => Math.max(0, s - 1))}>◀ Önceki Sektör</button>
          <div className="sector-input-group">
            <label>Sektör:</label>
            <input 
              type="number" 
              value={sector} 
              onChange={(e) => setSector(Number(e.target.value))}
              min="0"
              className="sector-input"
            />
          </div>
          <button className="btn-secondary" onClick={() => setSector(s => s + 1)}>Sonraki Sektör ▶</button>
          <button className="btn-primary" onClick={() => fetchSector(sector)}>Git</button>
        </div>
      </div>

      <div className="entropy-indicator">
        <span style={{ fontWeight: 'bold', color: 'var(--text-main)' }}>Sector Entropy:</span>
        <div className="entropy-bar-bg">
          <div className={`entropy-bar-fill ${entropyClass}`} style={{ width: `${entropyRatio}%` }}></div>
        </div>
        <span style={{ minWidth: '60px', textAlign: 'right', color: 'var(--text-muted)' }}>
          {currentEntropy.toFixed(2)} / 8
        </span>
        <span style={{ fontSize: '0.8rem', color: currentEntropy > 7.0 ? 'var(--alert-red)' : 'var(--text-muted)' }}>
          {currentEntropy > 7.0 ? '(Encrypted/Compressed)' : '(Plain/Structured)'}
        </span>
      </div>

      <div className={`hex-view glass-panel ${currentEntropy > 7.0 ? 'entropy-mode' : ''}`}>
        {loading ? (
          <div className="loading-state">
            <div className="spinner"></div>
            <p>Sektör verisi okunuyor...</p>
          </div>
        ) : (
          <div className="hex-grid">
            <div className="hex-header">
              <div className="offset-col">Offset</div>
              <div className="data-col">
                {Array.from({ length: 16 }).map((_, i) => <span key={i}>{toHex(i)}</span>)}
              </div>
              <div className="ascii-col">ASCII</div>
            </div>
            
            <div className="hex-body">
              {Array.from({ length: 32 }).map((_, row) => {
                const rowOffset = row * 16
                const rowData = data.slice(rowOffset, rowOffset + 16)
                
                // Pad if incomplete
                while (rowData.length < 16) rowData.push(0)

                return (
                  <div key={row} className="hex-row">
                    <div className="offset-col">{toHex(rowOffset, 4)}</div>
                    <div className="data-col">
                      {rowData.map((byte, col) => (
                        <span key={col} className={byte === 0 ? 'zero-byte' : 'active-byte'}>
                          {toHex(byte)}
                        </span>
                      ))}
                    </div>
                    <div className="ascii-col">
                      {rowData.map((byte, col) => (
                        <span key={col}>{toAscii(byte)}</span>
                      ))}
                    </div>
                  </div>
                )
              })}
            </div>
          </div>
        )}
      </div>
    </div>
  )
}

export default HexEditor
