import React, { useState, useEffect, useRef } from 'react'
import './ImagerView.css'
import { HardDrive, Save, Activity, CheckCircle, Square, Server, Play } from 'lucide-react'
import { ewfWillRotateSegments } from '../../../shared/ewf-limits'
import { defaultImagerUseVolume } from '../../../shared/win32-volume-path'
import { HEX_RAID_DRIVE_INDEX, probeRaidState } from '../../../shared/hex-read'
import InlineAlert from '../InlineAlert'
import { useI18n, tFormat, formatInt } from '../../i18n'

interface DriveInfo {
  index: number
  model: string
  serial: string
  sizeBytes: number
  sectorSize: number
  type: string
}

interface ImagerViewProps {
  /** CA-032: lifted to App — survives navigation while the main process images. */
  imagingActive: boolean
  onImagingStateChange: (active: boolean) => void
  /** Scan-bound `\\.\X:` — optional volume clone instead of PhysicalDrive. */
  scanVolumePath?: string
  /** >1 evidence disks: PhysicalDrive is one extent; default the volume device. */
  spanned?: boolean
}

/** Status line stored as an i18n key (+ optional engine error detail), same
 *  shape as App's ScanStatusMessage — rendered through t()/tFormat so a
 *  mid-session language switch re-translates instead of showing stale copy. */
interface ImagerStatusMessage { key: string; err?: string }

function ImagerView({ imagingActive, onImagingStateChange, scanVolumePath, spanned }: ImagerViewProps): React.ReactElement {
  const { t } = useI18n()
  const [drives, setDrives] = useState<DriveInfo[]>([])
  const [selectedDrive, setSelectedDrive] = useState<number | ''>('')
  const [destPath, setDestPath] = useState<string>('')

  const [imaging, setImaging] = useState(imagingActive)
  const [progress, setProgress] = useState({ current: 0, total: 0 })
  const [status, setStatus] = useState<ImagerStatusMessage | null>(imagingActive ? { key: 'imager.running' } : null)
  const [elapsed, setElapsed] = useState(0)
  const [latencies, setLatencies] = useState<number[]>([]) // EKG Chart Data
  const [format, setFormat] = useState<'raw' | 'ewf'>('raw')
  const [imageMd5, setImageMd5] = useState<string>('')
  const [formError, setFormError] = useState<string | null>(null)
  const [ewfConfirmOpen, setEwfConfirmOpen] = useState(false)
  const [useVolume, setUseVolume] = useState(() => defaultImagerUseVolume(scanVolumePath))
  const [raid, setRaid] = useState<{ active: boolean; capacity: number } | null>(null)
  const [raidStateError, setRaidStateError] = useState<string | null>(null)

  const timerRef = useRef<NodeJS.Timeout | null>(null)
  const lastProgressTimeRef = useRef<number>(0)
  // A trailing progress event after the user cancelled must not flip the
  // status back to "complete"/"failed" — the cancel message stays.
  const cancelledRef = useRef(false)

  useEffect(() => {
    setUseVolume(defaultImagerUseVolume(scanVolumePath))
  }, [scanVolumePath])

  useEffect(() => {
    // Load drives (guard: a late response after unmount must not setState)
    if (window.api && window.api.listDrives) {
      let alive = true
      window.api.listDrives().then((list) => {
        if (alive) {
          setDrives(list)
          setFormError(null)
        }
      }).catch(() => {
        if (alive) setFormError(t('imager.drivesFailed'))
      })
      return () => { alive = false }
    }
  }, [])

  useEffect(() => {
    let alive = true
    void probeRaidState(window.api?.getRaidState).then((probe) => {
      if (!alive) return
      if (probe.status === 'unread') {
        setRaid(null)
        setRaidStateError(t('imager.raidStateFailed'))
        return
      }
      setRaidStateError(null)
      setRaid(probe.state.active ? { active: true, capacity: probe.state.capacity } : null)
    })
    return () => { alive = false }
  }, [t])

  useEffect(() => {
    let cleanupProgress: (() => void) | undefined
    if (window.api && window.api.onImagingProgress) {
      cleanupProgress = window.api.onImagingProgress((data: { current: number, total: number, md5?: string, error?: string, status?: 'cancelled' }) => {
        if (data.status === 'cancelled') {
          // Explicit terminal marker from main on stop — the native abort path
          // emits no completion tick, so this is the authoritative end event.
          cancelledRef.current = false
          setStatus({ key: 'imager.cancelled' })
          setImaging(false)
          onImagingStateChange(false)
          if (timerRef.current) clearInterval(timerRef.current)
          return
        }
        if (cancelledRef.current) return
        if (data.total === 0) {
          // CA-034: surface the real reason when the main process sent one.
          setStatus(data.error ? { key: 'imager.failedWith', err: data.error } : { key: 'imager.failed' })
          setImaging(false)
          onImagingStateChange(false)
          if (timerRef.current) clearInterval(timerRef.current)
          return
        }
        setProgress(data)

        setLatencies(prev => {
          const now = Date.now();
          const lastTime = lastProgressTimeRef.current || now;
          const delta = now - lastTime;
          lastProgressTimeRef.current = now;

          // Avoid 0ms spikes on first run
          const finalLatency = delta > 0 && delta < 1000 ? delta : 15;

          const next = [...prev, finalLatency];
          if (next.length > 50) next.shift(); // Keep last 50 reads
          return next;
        });

        if (data.current >= data.total && data.total > 0) {
          setStatus({ key: 'imager.done' })
          setImaging(false)
          onImagingStateChange(false)
          if (data.md5) setImageMd5(data.md5)
          if (timerRef.current) clearInterval(timerRef.current)
        }
      })
    }

    return () => {
      if (cleanupProgress) cleanupProgress()
    }
  }, [onImagingStateChange])

  useEffect(() => {
    if (imaging) {
      if (timerRef.current) clearInterval(timerRef.current)
      timerRef.current = setInterval(() => setElapsed(prev => prev + 1), 1000)
    }
    return () => {
      if (timerRef.current) clearInterval(timerRef.current)
      timerRef.current = null
    }
  }, [imaging])

  const beginImaging = () => {
    setFormError(null)
    if (!window.api?.startImaging) {
      setFormError(t('imager.noApi'))
      return
    }
    if (selectedDrive === '' || selectedDrive === undefined) {
      setFormError(t('imager.selectDrive'))
      return
    }
    cancelledRef.current = false
    setImaging(true)
    setStatus({ key: 'imager.starting' })
    onImagingStateChange(true)
    setProgress({ current: 0, total: 0 })
    setElapsed(0)
    setImageMd5('')

    if (window.api && window.api.startImaging) {
      const raidSource = selectedDrive === HEX_RAID_DRIVE_INDEX
      const vp = !raidSource && useVolume && scanVolumePath ? scanVolumePath : undefined
      window.api.startImaging(Number(selectedDrive), destPath, format, vp)
    } else {
      setImaging(false)
      onImagingStateChange(false)
    }
  }

  const handleStartImaging = () => {
    if (selectedDrive === '' || destPath.trim() === '') {
      setFormError(t('imager.needSourceDest'))
      return
    }

    if (format === 'ewf') {
      const sizeBytes = selectedDrive === HEX_RAID_DRIVE_INDEX
        ? raid?.capacity
        : drives.find(d => d.index === Number(selectedDrive))?.sizeBytes
      if (sizeBytes != null && ewfWillRotateSegments(sizeBytes)) {
        setEwfConfirmOpen(true)
        return
      }
    }
    beginImaging()
  }
  
  const handleStopImaging = () => {
    cancelledRef.current = true
    if (window.api && window.api.stopImaging) {
      window.api.stopImaging()
    }
    setImaging(false)
    onImagingStateChange(false)
    setStatus({ key: 'imager.cancelled' })
  }

  const formatTime = (seconds: number) => {
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = seconds % 60
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }

  const percent = progress.total > 0 ? Math.floor((progress.current / progress.total) * 100) : 0
  // Key comparison, not localized-text matching — survives a language switch.
  const isImagingDone = status?.key === 'imager.done'
  const statusLabel = status ? tFormat(status.key, status.err != null ? { err: status.err } : {}) : ''
  const raidSource = selectedDrive === HEX_RAID_DRIVE_INDEX
  const selectedDriveInfo = selectedDrive === '' || raidSource
    ? undefined
    : drives.find(d => d.index === Number(selectedDrive))
  const selectedSizeBytes = raidSource ? raid?.capacity : selectedDriveInfo?.sizeBytes
  const showEwfSegmentWarning =
    format === 'ewf' && selectedSizeBytes != null && ewfWillRotateSegments(selectedSizeBytes)

  return (
    <div className="imager-view">
      <div className="imager-header glass-panel">
        <div className="examiner-icon">
          <Save size={32} color="var(--accent-blue)" />
        </div>
        <div>
          <h2>{t('imager.title')}</h2>
          <p>{t('imager.subtitle')}</p>
        </div>
      </div>

      <div className="imager-content glass-panel">
        {formError && (
          <InlineAlert variant="error" testId="imager-form-error" onDismiss={() => setFormError(null)}>{formError}</InlineAlert>
        )}
        {raidStateError && (
          <InlineAlert variant="error" testId="imager-raid-state-error">{raidStateError}</InlineAlert>
        )}
        {!imaging && drives.length === 0 && !raid?.active && (
          <div className="examiner-empty" role="status" data-testid="imager-empty">
            <h3>{t('imager.noDriveTitle')}</h3>
            <p>{t('imager.noDriveBody')}</p>
          </div>
        )}
        {ewfConfirmOpen && (
          <InlineAlert variant="warning" title={t('imager.ewfConfirmTitle')}>
            {t('imager.ewfConfirmBody')}
            <div className="imager-alert-actions">
              <button type="button" className="btn-primary" onClick={() => { setEwfConfirmOpen(false); beginImaging() }}>{t('dash.resume')}</button>
              <button type="button" className="btn-secondary" onClick={() => setEwfConfirmOpen(false)}>{t('ssd.cancel')}</button>
            </div>
          </InlineAlert>
        )}
        <div className="form-group">
          <label>
            <Server size={16} /> {t('imager.sourceLabel')}
          </label>
          <select
            className="form-select"
            data-testid="imager-source-select"
            value={selectedDrive}
            onChange={(e) => setSelectedDrive(e.target.value === '' ? '' : Number(e.target.value))}
            disabled={imaging}
          >
            <option value="">{t('imager.selectPlaceholder')}</option>
            {raid?.active && (
              <option data-testid="imager-raid-source" value={HEX_RAID_DRIVE_INDEX}>
                {t('hex.raidArray')} ({Math.floor(raid.capacity / (1024 * 1024 * 1024))} GB)
              </option>
            )}
            {drives.map(d => (
              <option key={d.index} value={d.index}>
                {tFormat('drive.physical', { n: String(d.index) })} - {d.model} ({Math.floor(d.sizeBytes / (1024*1024*1024))} GB)
              </option>
            ))}
          </select>
          {raidSource && raid?.active && (
            <p className="imager-volume-note">{t('imager.raidNote')}</p>
          )}
          {scanVolumePath && !raidSource && (
            <div className="imager-volume-bind">
              <p className="imager-volume-note">{spanned ? t('imager.spannedNote') : t('imager.volumeNote')}</p>
              <label className="imager-volume-check">
                <input
                  type="checkbox"
                  data-testid="imager-use-volume"
                  checked={useVolume}
                  disabled={imaging}
                  onChange={(e) => setUseVolume(e.target.checked)}
                />
                {tFormat('imager.useVolume', { p: scanVolumePath })}
              </label>
            </div>
          )}
        </div>

        <div className="form-group">
          <label>{t('imager.formatLabel')}</label>
          <select
            value={format}
            onChange={(e) => setFormat(e.target.value as 'raw' | 'ewf')}
            className="form-select"
            disabled={imaging}
          >
            <option value="raw">{t('imager.formatRaw')}</option>
            <option value="ewf">{t('imager.formatEwf')}</option>
          </select>
          {showEwfSegmentWarning && (
            <p role="status" className="imager-ewf-note">
              {t('imager.ewfMultiSegment')}
            </p>
          )}
        </div>

        <div className="form-group">
          <label>{t('imager.destLabel')}</label>
          <div className="path-input-group">
            <input 
              type="text" 
              className="form-input"
              placeholder={t('imager.destPlaceholder')}
              value={destPath}
              readOnly
              disabled={imaging}
            />
            <button
              type="button"
              className="btn-secondary"
              disabled={imaging}
              onClick={async () => {
                if (!window.api?.pickSaveImage) return
                const picked = await window.api.pickSaveImage(format)
                if (picked) setDestPath(picked)
              }}
            >
              {t('imager.browse')}
            </button>
          </div>
        </div>

        <div className="form-actions">
          {!imaging ? (
            <button className="btn-primary start-btn" onClick={handleStartImaging}>
              <Play size={18} fill="currentColor" /> {t('imager.start')}
            </button>
          ) : (
            <button className="btn-secondary stop-btn" onClick={handleStopImaging}>
              <Square size={18} fill="currentColor" /> {t('imager.cancelBtn')}
            </button>
          )}
        </div>

        {(imaging || status) && (
          <div className="imager-progress-card glass-panel">
            <div className="progress-head">
              <span className={`progress-status${isImagingDone ? ' is-done' : ''}`}>
                {isImagingDone ? <CheckCircle size={18} /> : <Activity size={18} />} {statusLabel}
              </span>
              <span className="progress-elapsed">{tFormat('imager.elapsed', { t: formatTime(elapsed) })}</span>
            </div>
            
            <div className="progress-labels">
              <span>{tFormat('imager.sectorProgress', { cur: formatInt(progress.current), total: progress.total ? formatInt(progress.total) : '?' })}</span>
              <span>{tFormat('common.percent', { n: String(percent) })}</span>
            </div>
            <div className="progress-bar-bg">
              <div className="progress-bar-fill" style={{ width: `${percent}%` }}></div>
            </div>

            {imageMd5 && (
              <div className="imager-md5">
                <div className="imager-md5-label">
                  {t('imager.md5Title')}
                </div>
                <div className="imager-md5-value">
                  MD5: {imageMd5}
                </div>
              </div>
            )}

            {/* Predictive Latency Pulse Chart */}
            <div className="latency-chart-container">
              <div className="latency-chart-head">
                <span className="latency-chart-label">
                  <Activity size={14} /> {t('imager.latencyChart')}
                </span>
                <span className={`latency-chart-value${latencies[latencies.length - 1] > 100 ? ' is-high' : ''}`}>
                  {tFormat('imager.instant', { n: String(latencies.length > 0 ? latencies[latencies.length - 1] : 0) })}
                </span>
              </div>
              <div className="latency-chart">
                {latencies.map((val, idx) => {
                  const heightPct = Math.min(100, (val / 200) * 100);
                  const isHigh = val > 100;
                  return (
                    <div 
                      key={idx} 
                      className={`latency-bar${isHigh ? ' is-high' : ''}`}
                      style={{ height: `${heightPct}%` }}
                      title={`${val} ms`}
                    />
                  )
                })}
              </div>
            </div>
          </div>
        )}
      </div>
    </div>
  )
}

export default ImagerView
