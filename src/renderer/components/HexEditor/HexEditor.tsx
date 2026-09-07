import React, { useState, useEffect, useRef } from 'react'
import './HexEditor.css'
import { Binary, ChevronLeft, ChevronRight, Search, Server } from 'lucide-react'
import { calculateEntropy, classifyEntropy } from '../../../shared/entropy'
import { useI18n, tFormat } from '../../i18n'

interface HexEditorProps {
  driveIndex?: number | null
  sectorSize?: number
  scanBusy?: boolean
}

// Static cache to preserve sector across unmounts
let globalSectorCache = 0;

function HexEditor({ driveIndex, sectorSize = 512, scanBusy }: HexEditorProps): React.ReactElement {
  const { t } = useI18n()
  const [sector, setSector] = useState(globalSectorCache)
  const [data, setData] = useState<number[]>([])
  const [readFailed, setReadFailed] = useState(false)
  const [readError, setReadError] = useState<string | null>(null)
  const [atDiskEnd, setAtDiskEnd] = useState(false)
  const [loading, setLoading] = useState(false)
  // Generation guard: rapid prev/next clicks fire overlapping reads; only the
  // latest one may paint the grid (out-of-order IPC would show the wrong
  // sector's bytes and dead-stale loading state).
  const fetchGenRef = useRef(0)

  // Update cache whenever sector changes
  useEffect(() => {
    globalSectorCache = sector;
  }, [sector]);

  const fetchSector = async (secIndex: number) => {
    if (driveIndex === undefined || driveIndex === null) return
    const gen = ++fetchGenRef.current
    if (scanBusy) {
      setReadFailed(true)
      setReadError(t('hex.busyError'))
      setData([])
      return
    }
    setLoading(true)
    try {
      const offset = secIndex * sectorSize
      if (window.api && window.api.readHexData) {
        const result = await window.api.readHexData(driveIndex, offset, sectorSize)
        if (gen !== fetchGenRef.current) return
        if (result.data && result.data.length > 0) {
          setReadFailed(false)
          setReadError(null)
          setData(result.data)
          // Short read = last sector of the drive.
          setAtDiskEnd(result.data.length < sectorSize)
        } else {
          setReadFailed(true)
          setReadError(result.error ?? t('hex.sectorReadFailed'))
          setData([])
          setAtDiskEnd(true)
        }
      }
    } catch (err) {
      console.error(err)
      if (gen !== fetchGenRef.current) return
      setReadFailed(true)
      setReadError(t('hex.readFailedShort'))
      setData([])
      setAtDiskEnd(true)
    } finally {
      if (gen === fetchGenRef.current) setLoading(false)
    }
  }

  useEffect(() => {
    if (driveIndex !== undefined && driveIndex !== null) {
      fetchSector(sector)
    }
  }, [driveIndex, sector, scanBusy])

  const currentEntropy = calculateEntropy(data);
  const entropyRatio = (currentEntropy / 8) * 100;
  const entropyClass = `entropy-${classifyEntropy(currentEntropy)}`;

  const toHex = (num: number, padding: number = 2) => num.toString(16).toUpperCase().padStart(padding, '0')
  const toAscii = (num: number) => (num >= 32 && num <= 126 ? String.fromCharCode(num) : '.')

  if (driveIndex === undefined || driveIndex === null) {
    return (
      <div className="hex-editor empty glass-panel" style={{ padding: '60px', textAlign: 'center', margin: '40px' }}>
        <Binary size={48} style={{ margin: '0 auto 16px', color: 'var(--panel-border)' }} />
        <h3 style={{ fontSize: '1.2rem', marginBottom: '8px' }}>{t('hex.noDriveTitle')}</h3>
        <p style={{ color: 'var(--text-muted)' }}>{t('hex.noDriveBody')}</p>
      </div>
    )
  }

  return (
    <div className="hex-editor">
      <div className="hex-toolbar glass-panel" style={{ display: 'flex', justifyContent: 'space-between', padding: '16px 24px', alignItems: 'center', marginBottom: '16px' }}>
        <div className="toolbar-info" style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
          <Server size={20} color="var(--accent-blue)" />
          <h3 style={{ fontSize: '1.1rem', margin: 0 }}>{tFormat('drive.physical', { n: String(driveIndex) })}</h3>
          <span className="badge" style={{ fontSize: '0.7rem', padding: '2px 6px', border: '1px solid var(--alert-red)', color: 'var(--alert-red)', borderRadius: '4px' }}>{t('hex.readonly')}</span>
        </div>
        <div className="sector-navigation" style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
          <button className="btn-secondary" onClick={() => setSector(s => Math.max(0, s - 1))} disabled={sector <= 0} style={{ padding: '6px 12px' }}><ChevronLeft size={16} /> {t('scan.prev')}</button>
          <div className="sector-input-group" style={{ display: 'flex', alignItems: 'center', gap: '8px', background: 'var(--well-bg)', padding: '4px 12px', borderRadius: '6px', border: '1px solid var(--panel-border)' }}>
            <label style={{ fontSize: '0.8rem', color: 'var(--text-muted)' }}>{t('scan.sector')}</label>
            <input
              type="number"
              defaultValue={sector}
              key={sector}
              onBlur={(e) => {
                // Number('') is 0 — an emptied field must not jump to sector 0.
                if (e.target.value === '') return
                const pending = Number(e.target.value)
                if (Number.isFinite(pending) && pending >= 0) setSector(pending)
              }}
              onKeyDown={(e) => {
                if (e.key === 'Enter') (e.currentTarget as HTMLInputElement).blur()
              }}
              min="0"
              style={{ background: 'transparent', border: 'none', color: 'var(--text-main)', width: '80px', fontFamily: 'monospace' }}
            />
          </div>
          <button className="btn-secondary" onClick={() => setSector(s => s + 1)} disabled={atDiskEnd || loading} style={{ padding: '6px 12px' }}>{t('scan.next')} <ChevronRight size={16} /></button>
          <button className="btn-primary" onClick={() => fetchSector(sector)} style={{ padding: '6px 12px' }}><Search size={16} /> {t('hex.go')}</button>
        </div>
      </div>

      {readFailed && (
        <div className="glass-panel" role="alert" style={{ padding: '16px 24px', marginBottom: '16px', borderLeft: '4px solid var(--alert-red)' }}>
          {readError ?? t('hex.readFailedShort')} {t('hex.zeroGridNote')}
        </div>
      )}

      {!readFailed && (
      <div className="entropy-indicator glass-panel" style={{ padding: '16px 24px', display: 'flex', alignItems: 'center', gap: '16px', marginBottom: '16px' }}>
        <span style={{ fontSize: '0.9rem', fontWeight: 500, color: 'var(--text-main)', width: '120px' }}>{t('hex.entropyLabel')}</span>
        <div style={{ flex: 1, height: '6px', background: 'var(--panel-border)', borderRadius: '3px', overflow: 'hidden' }}>
          <div className={`entropy-bar-fill ${entropyClass}`} style={{ width: `${entropyRatio}%`, height: '100%', transition: 'width 0.3s ease', background: currentEntropy > 7.0 ? 'var(--alert-red)' : currentEntropy > 4.5 ? 'var(--warning-yellow)' : 'var(--success-green)' }}></div>
        </div>
        <span style={{ minWidth: '60px', textAlign: 'right', color: 'var(--text-muted)', fontFamily: 'monospace' }}>
          {currentEntropy.toFixed(2)} / 8
        </span>
        <span style={{ fontSize: '0.8rem', color: currentEntropy > 7.0 ? 'var(--alert-red)' : 'var(--text-muted)', width: '160px' }}>
          {currentEntropy > 7.0 ? t('hex.entropyHigh') : t('hex.entropyLow')}
        </span>
      </div>
      )}

      {/* Data Template Engine */}
      {data.length >= 512 && data[0] === 0x46 && data[1] === 0x49 && data[2] === 0x4C && data[3] === 0x45 && (
        <div className="template-panel glass-panel" style={{ marginBottom: '16px', padding: '16px', borderLeft: '4px solid #b700ff' }}>
          <h4 style={{ fontSize: '0.95rem', marginBottom: '8px' }}>{t('hex.mftTitle')}</h4>
          <div style={{ display: 'flex', gap: '24px', fontSize: '0.85rem', color: 'var(--text-muted)' }}>
            <div><strong style={{ color: 'var(--text-main)' }}>0x00:</strong> <span style={{color: '#b700ff'}}>"FILE"</span> {t('hex.signature')}</div>
            <div><strong style={{ color: 'var(--text-main)' }}>0x04:</strong> {data[4] + (data[5] << 8)} {t('hex.updateArrayOffset')}</div>
            <div><strong style={{ color: 'var(--text-main)' }}>0x16:</strong> {data[22] === 0x01 ? t('hex.inUse') : t('scan.deleted')}</div>
          </div>
        </div>
      )}

      {data.length >= 512 && data[0] === 0xEB && data[2] === 0x90 && (
        <div className="template-panel glass-panel" style={{ marginBottom: '16px', padding: '16px', borderLeft: '4px solid var(--accent-blue)' }}>
          <h4 style={{ fontSize: '0.95rem', marginBottom: '8px' }}>{t('hex.bootTitle')}</h4>
          <div style={{ display: 'flex', gap: '24px', fontSize: '0.85rem', color: 'var(--text-muted)' }}>
            <div><strong style={{ color: 'var(--text-main)' }}>0x03:</strong> <span style={{color: 'var(--accent-blue)'}}>{String.fromCharCode(...data.slice(3, 11))}</span> {t('hex.oemName')}</div>
            <div><strong style={{ color: 'var(--text-main)' }}>0x0B:</strong> {data[11] + (data[12] << 8)} {t('hex.bytesPerSector')}</div>
            <div><strong style={{ color: 'var(--text-main)' }}>0x0D:</strong> {data[13]} {t('hex.sectorsPerCluster')}</div>
          </div>
        </div>
      )}

      <div className={`hex-view glass-panel ${currentEntropy > 7.0 ? 'entropy-mode' : ''}`} style={{ padding: '16px', fontFamily: 'monospace', fontSize: '0.9rem' }}>
        {loading ? (
          <div className="loading-state" style={{ padding: '40px', textAlign: 'center' }}>
            <Search size={32} className="spinner" style={{ margin: '0 auto 16px', color: 'var(--accent-blue)' }} />
            <p>{t('hex.loading')}</p>
          </div>
        ) : readFailed || data.length === 0 ? (
          <div style={{ padding: '40px', textAlign: 'center', color: 'var(--text-muted)' }}>
            {t('hex.cannotDisplay')}
          </div>
        ) : (
          <div className="hex-grid" style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
            <div className="hex-header" style={{ display: 'flex', color: 'var(--text-muted)', marginBottom: '8px', paddingBottom: '8px', borderBottom: '1px solid var(--panel-border)' }}>
              <div className="offset-col" style={{ width: '80px' }}>{t('hex.offset')}</div>
              <div className="data-col" style={{ flex: 1, display: 'flex', gap: '8px' }}>
                {Array.from({ length: 16 }).map((_, i) => <span key={i} style={{ width: '20px', textAlign: 'center' }}>{toHex(i)}</span>)}
              </div>
              <div className="ascii-col" style={{ width: '160px', paddingLeft: '16px' }}>ASCII</div>
            </div>
            
            <div className="hex-body">
              {Array.from({ length: 32 }).map((_, row) => {
                const rowOffset = row * 16
                const rowData = data.slice(rowOffset, rowOffset + 16)
                while (rowData.length < 16) rowData.push(0)

                return (
                  <div key={row} className="hex-row" style={{ display: 'flex', padding: '2px 0' }}>
                    <div className="offset-col" style={{ width: '80px', color: 'var(--text-muted)' }}>{toHex(rowOffset, 4)}</div>
                    <div className="data-col" style={{ flex: 1, display: 'flex', gap: '8px' }}>
                      {rowData.map((byte, col) => (
                        <span key={col} style={{ width: '20px', textAlign: 'center', color: byte === 0 ? 'var(--text-muted)' : 'var(--text-main)', opacity: byte === 0 ? 0.3 : 1 }}>
                          {toHex(byte)}
                        </span>
                      ))}
                    </div>
                    <div className="ascii-col" style={{ width: '160px', paddingLeft: '16px', color: 'var(--text-muted)', letterSpacing: '1px' }}>
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
