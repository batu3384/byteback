import React, { useState, useEffect, useRef } from 'react'
import './HexEditor.css'
import { Binary, Bookmark, ChevronLeft, ChevronRight, Search, Server } from 'lucide-react'
import { calculateEntropy, classifyEntropy } from '../../../shared/entropy'
import { HEX_RAID_DRIVE_INDEX, probeRaidState, parseHexSearchNeedle, hexSearchHitSector, HEX_SEARCH_MAX_HITS, mftAttrTypeLabel } from '../../../shared/hex-read'
import { addHexMark, type HexMark } from '../../../shared/hex-marks'
import { useI18n, tFormat } from '../../i18n'
import InlineAlert from '../InlineAlert'

interface HexEditorProps {
  driveIndex?: number | null
  sectorSize?: number
  scanBusy?: boolean
  /** Scan-bound Windows volume device (`\\.\X:`). Examiner can switch I/O. */
  volumePath?: string
  /** DriveCard opened hex as a PhysicalDrive dump — default off the volume device. */
  forceDisk?: boolean
  /** Results "show MFT" jumps here; HexEditor loads that record on mount. */
  initialMftRef?: number
}

type HexSource = 'disk' | 'volume' | 'raid'

// Static cache to preserve sector across unmounts
let globalSectorCache = 0;

function HexEditor({ driveIndex, sectorSize = 512, scanBusy, volumePath, forceDisk, initialMftRef }: HexEditorProps): React.ReactElement {
  const { t } = useI18n()
  const [sector, setSector] = useState(globalSectorCache)
  const [data, setData] = useState<number[]>([])
  const [readFailed, setReadFailed] = useState(false)
  const [readError, setReadError] = useState<string | null>(null)
  const [atDiskEnd, setAtDiskEnd] = useState(false)
  const [loading, setLoading] = useState(false)
  const [raidActive, setRaidActive] = useState(false)
  const [raidStateUnread, setRaidStateUnread] = useState(false)
  const [source, setSource] = useState<HexSource>('disk')
  const [searchQuery, setSearchQuery] = useState('')
  const [searchHits, setSearchHits] = useState<number[]>([])
  const [searchUnread, setSearchUnread] = useState(false)
  const [searchError, setSearchError] = useState<string | null>(null)
  const [searching, setSearching] = useState(false)
  const [searched, setSearched] = useState(false)
  const [mftRefInput, setMftRefInput] = useState('0')
  const [mftUnread, setMftUnread] = useState(false)
  const [mftError, setMftError] = useState<string | null>(null)
  const [mftView, setMftView] = useState<{
    signature: string
    flags: number
    attrs: { type: number; name: string; resident: boolean }[]
  } | null>(null)
  const [marks, setMarks] = useState<HexMark[]>([])
  // Generation guard: rapid prev/next clicks fire overlapping reads; only the
  // latest one may paint the grid (out-of-order IPC would show the wrong
  // sector's bytes and dead-stale loading state).
  const fetchGenRef = useRef(0)

  const canHexDisk = driveIndex !== undefined && driveIndex !== null && driveIndex >= 0
  const raidIntended = driveIndex === HEX_RAID_DRIVE_INDEX
  const canHexRaid = raidActive || raidIntended
  const canHexVolume = !!volumePath
  const boundDriveIndex = source === 'raid' ? HEX_RAID_DRIVE_INDEX : (driveIndex ?? 0)
  const boundVolume = source === 'volume' && volumePath ? volumePath : ''
  const visibleMarks = marks.filter((m) => m.driveIndex === boundDriveIndex && m.volumePath === boundVolume)

  useEffect(() => {
    let alive = true
    void window.api?.getHexMarks?.().then((loaded) => {
      if (!alive || !Array.isArray(loaded)) return
      setMarks(loaded)
    }).catch(() => { /* sidecar missing is empty marks, not a hex read failure */ })
    return () => { alive = false }
  }, [])

  // Update cache whenever sector changes
  useEffect(() => {
    globalSectorCache = sector;
  }, [sector]);

  useEffect(() => {
    let alive = true
    void probeRaidState(window.api?.getRaidState).then((probe) => {
      if (!alive) return
      if (probe.status === 'unread') {
        setRaidActive(false)
        setRaidStateUnread(true)
        return
      }
      setRaidStateUnread(false)
      setRaidActive(!!probe.state.active)
    })
    return () => { alive = false }
  }, [driveIndex])

  useEffect(() => {
    if (raidIntended || (canHexRaid && !canHexDisk && !volumePath)) {
      setSource('raid')
    } else if (volumePath && !forceDisk) {
      setSource('volume')
    } else if (canHexDisk) {
      setSource('disk')
    } else if (canHexRaid) {
      setSource('raid')
    }
  }, [volumePath, forceDisk, driveIndex, canHexRaid, canHexDisk, raidIntended])

  const fetchSector = async (secIndex: number) => {
    const idx = source === 'raid' ? HEX_RAID_DRIVE_INDEX : driveIndex
    if (idx === undefined || idx === null) return
    if (source !== 'raid' && source !== 'volume' && idx < 0) return
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
        const boundVolume = source === 'volume' && volumePath ? volumePath : undefined
        const result = await window.api.readHexData(idx, offset, sectorSize, boundVolume)
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

  const runSearch = async () => {
    const idx = source === 'raid' ? HEX_RAID_DRIVE_INDEX : driveIndex
    if (idx === undefined || idx === null) return
    if (source !== 'raid' && source !== 'volume' && idx < 0) return
    if (scanBusy) {
      setSearchError(t('hex.busyError'))
      return
    }
    const parsed = parseHexSearchNeedle(searchQuery)
    if (!parsed.ok) {
      setSearchHits([])
      setSearchUnread(false)
      setSearched(false)
      setSearchError(t('hex.searchInvalid'))
      return
    }
    setSearching(true)
    setSearchError(null)
    setSearchUnread(false)
    try {
      if (!window.api?.searchHex) {
        setSearchError(t('hex.searchInvalid'))
        return
      }
      const boundVolume = source === 'volume' && volumePath ? volumePath : undefined
      const result = await window.api.searchHex(idx, parsed.bytes, HEX_SEARCH_MAX_HITS, boundVolume)
      setSearchHits(Array.isArray(result.hits) ? result.hits : [])
      setSearchUnread(!!result.unread)
      setSearched(true)
      setSearchError(result.error ?? null)
    } catch (err) {
      console.error(err)
      setSearchHits([])
      setSearched(true)
      setSearchError(t('hex.readFailedShort'))
    } finally {
      setSearching(false)
    }
  }

  const loadMftRecord = async (refOverride?: number) => {
    const idx = source === 'raid' ? HEX_RAID_DRIVE_INDEX : driveIndex
    if (idx === undefined || idx === null) return
    if (source !== 'raid' && source !== 'volume' && idx < 0) return
    if (scanBusy) {
      setMftError(t('hex.busyError'))
      return
    }
    const ref = refOverride !== undefined ? refOverride : Number(mftRefInput)
    if (!Number.isInteger(ref) || ref < 0) {
      setMftError(t('hex.searchInvalid'))
      return
    }
    if (refOverride !== undefined) setMftRefInput(String(refOverride))
    setMftError(null)
    setMftUnread(false)
    try {
      if (!window.api?.getMftRecord) {
        setMftError(t('hex.readFailedShort'))
        return
      }
      const boundVolume = source === 'volume' && volumePath ? volumePath : undefined
      const result = await window.api.getMftRecord(idx, ref, boundVolume)
      setMftUnread(!!result.unread)
      if (!result.ok) {
        setMftView(null)
        setMftError(result.error ?? (result.unread ? t('hex.mftUnread') : t('hex.readFailedShort')))
        return
      }
      setMftView({
        signature: result.signature ?? '',
        flags: result.flags ?? 0,
        attrs: Array.isArray(result.attrs) ? result.attrs : [],
      })
      if (typeof result.byteOffset === 'number' && Number.isFinite(result.byteOffset)) {
        setSector(hexSearchHitSector(result.byteOffset, sectorSize))
      }
    } catch (err) {
      console.error(err)
      setMftView(null)
      setMftError(t('hex.readFailedShort'))
    }
  }

  const persistMarks = async (next: HexMark[]) => {
    setMarks(next)
    try {
      await window.api?.setHexMarks?.(next)
    } catch {
      /* local list still updated; sidecar write is best-effort */
    }
  }

  const addCurrentMark = () => {
    if (!Number.isInteger(boundDriveIndex)) return
    void persistMarks(addHexMark(marks, {
      driveIndex: boundDriveIndex,
      volumePath: boundVolume,
      sector,
      label: '',
    }))
  }

  useEffect(() => {
    const ready = source === 'raid' ? canHexRaid : source === 'volume' ? !!volumePath : canHexDisk
    if (ready) fetchSector(sector)
  }, [driveIndex, sector, scanBusy, source, volumePath, canHexRaid, canHexDisk])

  useEffect(() => {
    if (typeof initialMftRef !== 'number' || !Number.isInteger(initialMftRef) || initialMftRef < 0) return
    const ready = source === 'raid' ? canHexRaid : source === 'volume' ? !!volumePath : canHexDisk
    if (!ready) return
    void loadMftRecord(initialMftRef)
  }, [initialMftRef, driveIndex, volumePath, source, scanBusy, canHexRaid, canHexDisk])


  const currentEntropy = calculateEntropy(data);
  const entropyRatio = (currentEntropy / 8) * 100;
  const entropyClass = `entropy-${classifyEntropy(currentEntropy)}`;

  const toHex = (num: number, padding: number = 2) => num.toString(16).toUpperCase().padStart(padding, '0')
  const toAscii = (num: number) => (num >= 32 && num <= 126 ? String.fromCharCode(num) : '.')

  const raidStateBanner = raidStateUnread ? (
    <InlineAlert variant="error" testId="hex-raid-state-error">{t('hex.raidStateFailed')}</InlineAlert>
  ) : null

  if (!canHexDisk && !canHexRaid && !canHexVolume) {
    return (
      <>
        {raidStateBanner}
        <div className="hex-editor empty glass-panel examiner-empty" data-testid="hex-empty" role="status">
        <div className="examiner-icon neutral" aria-hidden="true">
          <Binary size={28} color="var(--text-main)" />
        </div>
        <h3>{t('hex.noDriveTitle')}</h3>
        <p>{t('hex.noDriveBody')}</p>
      </div>
      </>
    )
  }

  const showSourceSelect =
    [canHexDisk, !!volumePath, canHexRaid].filter(Boolean).length > 1

  return (
    <div className="hex-editor">
      {raidStateBanner}
      <div className="hex-toolbar glass-panel">
        <div className="toolbar-info">
          <Server size={20} color="var(--accent-blue)" aria-hidden="true" />
          {showSourceSelect ? (
            <label className="hex-source-label">
              <span className="sr-only">{t('hex.sourceLabel')}</span>
              <select
                className="hex-source"
                data-testid="hex-source"
                value={source}
                onChange={(e) => setSource(e.target.value as HexSource)}
                aria-label={t('hex.sourceLabel')}
              >
                {canHexDisk && (
                  <option value="disk">{tFormat('drive.physical', { n: String(driveIndex) })}</option>
                )}
                {volumePath && (
                  <option value="volume">{tFormat('hex.volumeDevice', { p: volumePath })}</option>
                )}
                {canHexRaid && (
                  <option value="raid">{t('hex.raidArray')}</option>
                )}
              </select>
            </label>
          ) : (
            <h3>{source === 'raid' ? t('hex.raidArray') : tFormat('drive.physical', { n: String(driveIndex) })}</h3>
          )}
          <span className="badge hex-readonly">{t('hex.readonly')}</span>
        </div>
        <div className="sector-navigation">
          <button type="button" className="btn-secondary" onClick={() => setSector(s => Math.max(0, s - 1))} disabled={sector <= 0}><ChevronLeft size={16} aria-hidden="true" /> {t('scan.prev')}</button>
          <div className="sector-input-group">
            <label htmlFor="hex-sector">{t('scan.sector')}</label>
            <input
              id="hex-sector"
              className="sector-input"
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
            />
          </div>
          <button type="button" className="btn-secondary" onClick={() => setSector(s => s + 1)} disabled={atDiskEnd || loading}>{t('scan.next')} <ChevronRight size={16} aria-hidden="true" /></button>
          <button type="button" className="btn-primary" onClick={() => fetchSector(sector)}><Search size={16} aria-hidden="true" /> {t('hex.go')}</button>
        </div>
      </div>

      <div className="hex-search glass-panel">
        <label htmlFor="hex-search-query">{t('hex.search')}</label>
        <input
          id="hex-search-query"
          data-testid="hex-search-query"
          className="sector-input hex-search-input"
          type="text"
          value={searchQuery}
          placeholder={t('hex.searchPlaceholder')}
          onChange={(e) => setSearchQuery(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter') void runSearch()
          }}
          aria-label={t('hex.search')}
        />
        <button
          type="button"
          className="btn-primary"
          data-testid="hex-search-go"
          onClick={() => void runSearch()}
          disabled={searching || !!scanBusy}
        >
          <Search size={16} aria-hidden="true" /> {searching ? t('hex.searching') : t('hex.search')}
        </button>
      </div>

      {searchUnread && (
        <InlineAlert variant="warning" testId="hex-search-unread">{t('hex.searchUnread')}</InlineAlert>
      )}
      {searchError && (
        <InlineAlert variant="error">{searchError}</InlineAlert>
      )}
      {searched && searchHits.length === 0 && !searchError && (
        <p className="hex-search-empty" data-testid="hex-search-no-hits" role="status">{t('hex.noHits')}</p>
      )}
      {searchHits.length > 0 && (
        <ul className="hex-search-hits glass-panel" data-testid="hex-search-hits">
          {searchHits.map((off, i) => (
            <li key={`${off}:${i}`}>
              <button
                type="button"
                data-testid={`hex-search-hit-${i}`}
                onClick={() => setSector(hexSearchHitSector(off, sectorSize))}
              >
                0x{off.toString(16).toUpperCase()} ({off})
              </button>
            </li>
          ))}
        </ul>
      )}

      <div className="hex-search glass-panel">
        <button
          type="button"
          className="btn-secondary"
          data-testid="hex-mark-add"
          onClick={addCurrentMark}
        >
          <Bookmark size={16} aria-hidden="true" /> {t('hex.markAdd')}
        </button>
      </div>
      {visibleMarks.length > 0 && (
        <ul className="hex-search-hits glass-panel" data-testid="hex-marks" aria-label={t('hex.marks')}>
          {visibleMarks.map((m, i) => (
            <li key={`${m.driveIndex}:${m.volumePath}:${m.sector}`}>
              <button
                type="button"
                data-testid={`hex-mark-${i}`}
                onClick={() => setSector(m.sector)}
              >
                {m.label || String(m.sector)}
              </button>
            </li>
          ))}
        </ul>
      )}

      <div className="hex-search glass-panel">
        <label htmlFor="hex-mft-ref">{t('hex.mftGo')}</label>
        <input
          id="hex-mft-ref"
          data-testid="hex-mft-ref"
          className="sector-input"
          type="number"
          min="0"
          value={mftRefInput}
          onChange={(e) => setMftRefInput(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter') void loadMftRecord()
          }}
          aria-label={t('hex.mftGo')}
        />
        <button
          type="button"
          className="btn-primary"
          data-testid="hex-mft-go"
          onClick={() => void loadMftRecord()}
          disabled={!!scanBusy}
        >
          {t('hex.mftGo')}
        </button>
      </div>
      {mftUnread && (
        <InlineAlert variant="warning" testId="hex-mft-unread">{t('hex.mftUnread')}</InlineAlert>
      )}
      {mftError && (
        <InlineAlert variant="error">{mftError}</InlineAlert>
      )}
      {mftView && (
        <div className="template-panel glass-panel" data-testid="hex-mft-view">
          <h4>{t('hex.mftTitle')}</h4>
          <div className="template-fields">
            <div><strong>{t('hex.signature')}</strong> <span className="field-accent">{mftView.signature}</span></div>
            <div><strong>flags</strong> 0x{mftView.flags.toString(16).toUpperCase()} {mftView.flags & 0x01 ? t('hex.inUse') : t('scan.deleted')}</div>
          </div>
          <ul className="hex-mft-attrs">
            {mftView.attrs.map((a, i) => (
              <li key={`${a.type}:${a.name}:${i}`} data-testid={`hex-mft-attr-${i}`}>
                {mftAttrTypeLabel(a.type)}{a.name ? `:${a.name}` : ''} {a.resident ? 'resident' : 'non-resident'}
              </li>
            ))}
          </ul>
        </div>
      )}

      {readFailed && (
        <InlineAlert variant="error">
          {readError ?? t('hex.readFailedShort')} {t('hex.zeroGridNote')}
        </InlineAlert>
      )}

      {!readFailed && (
      <div className="entropy-indicator glass-panel">
        <span className="entropy-label">{t('hex.entropyLabel')}</span>
        <div className="entropy-track" role="meter" aria-valuemin={0} aria-valuemax={8} aria-valuenow={Number(currentEntropy.toFixed(2))} aria-label={t('hex.entropyLabel')}>
          <div className={`entropy-bar-fill ${entropyClass}`} style={{ width: `${entropyRatio}%` }}></div>
        </div>
        <span className="entropy-value">
          {currentEntropy.toFixed(2)} / 8
        </span>
        <span className={`entropy-hint${currentEntropy > 7.0 ? ' is-high' : ''}`}>
          {currentEntropy > 7.0 ? t('hex.entropyHigh') : t('hex.entropyLow')}
        </span>
      </div>
      )}

      {data.length >= 512 && data[0] === 0x46 && data[1] === 0x49 && data[2] === 0x4C && data[3] === 0x45 && (
        <div className="template-panel glass-panel">
          <h4>{t('hex.mftTitle')}</h4>
          <div className="template-fields">
            <div><strong>0x00:</strong> <span className="field-accent">"FILE"</span> {t('hex.signature')}</div>
            <div><strong>0x04:</strong> {data[4] + (data[5] << 8)} {t('hex.updateArrayOffset')}</div>
            <div><strong>0x16:</strong> {data[22] === 0x01 ? t('hex.inUse') : t('scan.deleted')}</div>
          </div>
        </div>
      )}

      {data.length >= 512 && data[0] === 0xEB && data[2] === 0x90 && (
        <div className="template-panel glass-panel">
          <h4>{t('hex.bootTitle')}</h4>
          <div className="template-fields">
            <div><strong>0x03:</strong> <span className="field-accent">{String.fromCharCode(...data.slice(3, 11))}</span> {t('hex.oemName')}</div>
            <div><strong>0x0B:</strong> {data[11] + (data[12] << 8)} {t('hex.bytesPerSector')}</div>
            <div><strong>0x0D:</strong> {data[13]} {t('hex.sectorsPerCluster')}</div>
          </div>
        </div>
      )}

      <div className={`hex-view glass-panel ${currentEntropy > 7.0 ? 'entropy-mode' : ''}`}>
        {loading ? (
          <div className="hex-loading" role="status">
            <Search size={32} className="spinner" />
            <p>{t('hex.loading')}</p>
          </div>
        ) : readFailed || data.length === 0 ? (
          <div className="hex-cannot">
            {t('hex.cannotDisplay')}
          </div>
        ) : (
          <div className="hex-grid">
            <div className="hex-header">
              <div className="offset-col">{t('hex.offset')}</div>
              <div className="data-col">
                {Array.from({ length: 16 }).map((_, i) => <span key={i}>{toHex(i)}</span>)}
              </div>
              <div className="ascii-col">{t('hex.ascii')}</div>
            </div>
            
            <div className="hex-body">
              {Array.from({ length: 32 }).map((_, row) => {
                const rowOffset = row * 16
                const rowData = data.slice(rowOffset, rowOffset + 16)
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
