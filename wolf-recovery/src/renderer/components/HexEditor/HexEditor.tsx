import React, { useState, useEffect } from 'react'
import './HexEditor.css'

function HexEditor(): React.ReactElement {
  const [driveIndex, setDriveIndex] = useState(0)
  const [sectorOffset, setSectorOffset] = useState(0)
  const [hexData, setHexData] = useState<number[]>([])
  const [loading, setLoading] = useState(false)

  const fetchHexData = async () => {
    setLoading(true)
    try {
      if (window.api && window.api.readHexData) {
        const data = await window.api.readHexData(driveIndex, sectorOffset * 512, 512)
        setHexData(data)
      } else {
        // Fallback for UI testing
        setHexData(new Array(512).fill(0).map(() => Math.floor(Math.random() * 256)))
      }
    } catch (err) {
      console.error('Failed to read hex data', err)
      setHexData(new Array(512).fill(0))
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => {
    fetchHexData()
  }, [driveIndex, sectorOffset])

  const renderHexRow = (offset: number, rowData: number[]) => {
    const hexString = rowData.map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ')
    const asciiString = rowData.map(b => (b >= 32 && b <= 126) ? String.fromCharCode(b) : '.').join('')

    return (
      <div key={offset} className="hex-row">
        <span className="hex-offset">{(sectorOffset * 512 + offset).toString(16).padStart(8, '0').toUpperCase()}</span>
        <span className="hex-bytes">{hexString}</span>
        <span className="hex-ascii">{asciiString}</span>
      </div>
    )
  }

  const rows = []
  for (let i = 0; i < hexData.length; i += 16) {
    rows.push(renderHexRow(i, hexData.slice(i, i + 16)))
  }

  return (
    <div className="hex-editor">
      <div className="hex-toolbar">
        <div className="toolbar-group">
          <label>Drive Index:</label>
          <input 
            type="number" 
            value={driveIndex} 
            onChange={(e) => setDriveIndex(Number(e.target.value))}
            min="0"
          />
        </div>
        <div className="toolbar-group">
          <label>Sector:</label>
          <input 
            type="number" 
            value={sectorOffset} 
            onChange={(e) => setSectorOffset(Number(e.target.value))}
            min="0"
          />
        </div>
        <button className="btn-secondary" onClick={fetchHexData}>Refresh</button>
      </div>

      <div className="hex-content-container">
        {loading ? (
          <div className="hex-loading">Reading sectors...</div>
        ) : (
          <div className="hex-grid">
            <div className="hex-header-row">
              <span className="hex-offset-header">OFFSET</span>
              <span className="hex-bytes-header">
                00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
              </span>
              <span className="hex-ascii-header">ASCII</span>
            </div>
            <div className="hex-rows">
              {rows}
            </div>
          </div>
        )}
      </div>
    </div>
  )
}

export default HexEditor
