import React, { useState, useEffect, useRef } from 'react'
import './ImagerView.css'
import { HardDrive, Save, Activity, CheckCircle, Square, Server, Play } from 'lucide-react'
import { ewfWillRotateSegments } from '../../../shared/ewf-limits'
import InlineAlert from '../InlineAlert'
import { useI18n, tFormat } from '../../i18n'

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
}

function ImagerView({ imagingActive, onImagingStateChange }: ImagerViewProps): React.ReactElement {
  const { t } = useI18n()
  const [drives, setDrives] = useState<DriveInfo[]>([])
  const [selectedDrive, setSelectedDrive] = useState<number | ''>('')
  const [destPath, setDestPath] = useState<string>('')

  const [imaging, setImaging] = useState(imagingActive)
  const [progress, setProgress] = useState({ current: 0, total: 0 })
  const [status, setStatus] = useState<string>(imagingActive ? t('imager.running') : '')
  const [elapsed, setElapsed] = useState(0)
  const [latencies, setLatencies] = useState<number[]>([]) // EKG Chart Data
  const [format, setFormat] = useState<'raw' | 'ewf'>('raw')
  const [imageMd5, setImageMd5] = useState<string>('')
  const [formError, setFormError] = useState<string | null>(null)
  const [ewfConfirmOpen, setEwfConfirmOpen] = useState(false)

  const timerRef = useRef<NodeJS.Timeout | null>(null)
  const lastProgressTimeRef = useRef<number>(0)

  useEffect(() => {
    // Load drives
    if (window.api && window.api.listDrives) {
      window.api.listDrives().then(setDrives).catch(console.error)
    }
  }, [])

  useEffect(() => {
    let cleanupProgress: (() => void) | undefined
    if (window.api && window.api.onImagingProgress) {
      cleanupProgress = window.api.onImagingProgress((data: { current: number, total: number, md5?: string, error?: string }) => {
        if (data.total === 0) {
          // CA-034: surface the real reason when the main process sent one.
          setStatus(data.error ? tFormat('imager.failedWith', { err: data.error }) : t('imager.failed'))
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
          setStatus(t('imager.done'))
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
    setImaging(true)
    setStatus(t('imager.starting'))
    onImagingStateChange(true)
    setProgress({ current: 0, total: 0 })
    setElapsed(0)
    setImageMd5('')

    if (window.api && window.api.startImaging) {
      window.api.startImaging(Number(selectedDrive), destPath, format)
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
      const drive = drives.find(d => d.index === Number(selectedDrive))
      if (drive && ewfWillRotateSegments(drive.sizeBytes)) {
        setEwfConfirmOpen(true)
        return
      }
    }
    beginImaging()
  }
  
  const handleStopImaging = () => {
    if (window.api && window.api.stopImaging) {
      window.api.stopImaging()
    }
    setImaging(false)
    onImagingStateChange(false)
    setStatus(t('imager.cancelled'))
  }

  const formatTime = (seconds: number) => {
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = seconds % 60
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }

  const percent = progress.total > 0 ? Math.floor((progress.current / progress.total) * 100) : 0
  const selectedDriveInfo = selectedDrive === '' ? undefined : drives.find(d => d.index === Number(selectedDrive))
  const showEwfSegmentWarning =
    format === 'ewf' && !!selectedDriveInfo && ewfWillRotateSegments(selectedDriveInfo.sizeBytes)

  return (
    <div className="imager-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%', maxWidth: '800px', margin: '0 auto' }}>
      <div className="imager-header glass-panel" style={{ padding: '24px', display: 'flex', gap: '16px', alignItems: 'center' }}>
        <div style={{ background: 'rgba(59, 130, 246, 0.1)', padding: '16px', borderRadius: '12px' }}>
          <Save size={32} color="var(--accent-blue)" />
        </div>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>{t('imager.title')}</h2>
          <p style={{ color: 'var(--text-muted)' }}>{t('imager.subtitle')}</p>
        </div>
      </div>

      <div className="imager-content glass-panel" style={{ padding: '32px', display: 'flex', flexDirection: 'column', gap: '24px' }}>
        {formError && (
          <InlineAlert variant="error" onDismiss={() => setFormError(null)}>{formError}</InlineAlert>
        )}
        {ewfConfirmOpen && (
          <InlineAlert variant="warning" title={t('imager.ewfConfirmTitle')}>
            {t('imager.ewfConfirmBody')}
            <div style={{ marginTop: '12px', display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
              <button type="button" className="btn-primary" onClick={() => { setEwfConfirmOpen(false); beginImaging() }}>{t('dash.resume')}</button>
              <button type="button" className="btn-secondary" onClick={() => setEwfConfirmOpen(false)}>{t('ssd.cancel')}</button>
            </div>
          </InlineAlert>
        )}
        <div className="form-group" style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
          <label style={{ fontSize: '0.9rem', color: 'var(--text-muted)', fontWeight: 500, display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Server size={16} /> {t('imager.sourceLabel')}
          </label>
          <select
            className="form-select"
            style={{ width: '100%', padding: '12px 16px', background: 'rgba(0,0,0,0.2)', border: '1px solid var(--panel-border)', borderRadius: '8px', color: 'var(--text-main)', fontSize: '1rem' }}
            value={selectedDrive}
            onChange={(e) => setSelectedDrive(e.target.value === '' ? '' : Number(e.target.value))}
            disabled={imaging}
          >
            <option value="" style={{ background: 'var(--bg-surface)' }}>{t('imager.selectPlaceholder')}</option>
            {drives.map(d => (
              <option key={d.index} value={d.index} style={{ background: 'var(--bg-surface)' }}>
                {tFormat('drive.physical', { n: String(d.index) })} - {d.model} ({Math.floor(d.sizeBytes / (1024*1024*1024))} GB)
              </option>
            ))}
          </select>
        </div>

        <div className="form-group" style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
          <label style={{ fontSize: '0.9rem', color: 'var(--text-muted)', fontWeight: 500 }}>{t('imager.formatLabel')}</label>
          <select
            value={format}
            onChange={(e) => setFormat(e.target.value as 'raw' | 'ewf')}
            className="form-select"
            style={{ width: '100%', padding: '12px 16px', background: 'rgba(0,0,0,0.2)', border: '1px solid var(--panel-border)', borderRadius: '8px', color: 'var(--text-main)', fontSize: '1rem' }}
            disabled={imaging}
          >
            <option value="raw" style={{ background: 'var(--bg-surface)' }}>{t('imager.formatRaw')}</option>
            <option value="ewf" style={{ background: 'var(--bg-surface)' }}>{t('imager.formatEwf')}</option>
          </select>
          {showEwfSegmentWarning && (
            <p role="status" style={{ color: 'var(--warning-yellow)', fontSize: '0.85rem' }}>
              {t('imager.ewfMultiSegment')}
            </p>
          )}
        </div>

        <div className="form-group" style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
          <label style={{ fontSize: '0.9rem', color: 'var(--text-muted)', fontWeight: 500 }}>{t('imager.destLabel')}</label>
          <div style={{ display: 'flex', gap: '8px' }}>
            <input 
              type="text" 
              className="form-input"
              style={{ flex: 1, padding: '12px 16px', background: 'rgba(0,0,0,0.2)', border: '1px solid var(--panel-border)', borderRadius: '8px', color: 'var(--text-main)', fontSize: '1rem' }}
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
              style={{ padding: '0 16px', whiteSpace: 'nowrap' }}
            >
              {t('imager.browse')}
            </button>
          </div>
        </div>

        <div className="form-actions" style={{ display: 'flex', justifyContent: 'flex-end', marginTop: '16px' }}>
          {!imaging ? (
            <button className="btn-primary start-btn" onClick={handleStartImaging} style={{ padding: '12px 32px', fontSize: '1rem' }}>
              <Play size={18} fill="currentColor" /> {t('imager.start')}
            </button>
          ) : (
            <button className="btn-secondary stop-btn" onClick={handleStopImaging} style={{ padding: '12px 32px', fontSize: '1rem', color: 'var(--alert-red)', borderColor: 'rgba(239, 68, 68, 0.3)' }}>
              <Square size={18} fill="currentColor" /> {t('imager.cancelBtn')}
            </button>
          )}
        </div>

        {(imaging || status) && (
          <div className="imager-progress-card glass-panel" style={{ marginTop: '8px', padding: '24px', background: 'rgba(255,255,255,0.02)' }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '16px', alignItems: 'center' }}>
              <span style={{ fontWeight: 500, color: status.includes(t('imager.doneMarker')) ? 'var(--success-green)' : 'var(--accent-blue)', display: 'flex', alignItems: 'center', gap: '8px' }}>
                {status.includes(t('imager.doneMarker')) ? <CheckCircle size={18} /> : <Activity size={18} />} {status}
              </span>
              <span style={{ color: 'var(--text-muted)', fontFamily: 'monospace' }}>{tFormat('imager.elapsed', { t: formatTime(elapsed) })}</span>
            </div>
            
            <div className="progress-labels" style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.85rem', color: 'var(--text-muted)', marginBottom: '8px' }}>
              <span>{tFormat('imager.sectorProgress', { cur: progress.current.toLocaleString(), total: progress.total ? progress.total.toLocaleString() : '?' })}</span>
              <span>%{percent}</span>
            </div>
            <div className="progress-bar-bg" style={{ width: '100%', height: '8px', background: 'rgba(255,255,255,0.1)', borderRadius: '4px', overflow: 'hidden' }}>
              <div className="progress-bar-fill" style={{ width: `${percent}%`, height: '100%', background: 'var(--accent-blue)', transition: 'width 0.3s ease' }}></div>
            </div>

            {imageMd5 && (
              <div style={{ marginTop: '16px', padding: '12px 16px', background: 'rgba(16, 185, 129, 0.08)', border: '1px solid rgba(16, 185, 129, 0.3)', borderRadius: '8px' }}>
                <div style={{ fontSize: '0.8rem', color: 'var(--success-green)', marginBottom: '6px', fontWeight: 500 }}>
                  {t('imager.md5Title')}
                </div>
                <div style={{ fontFamily: 'monospace', fontSize: '0.85rem', color: 'var(--text-main)', wordBreak: 'break-all', userSelect: 'all' }}>
                  MD5: {imageMd5}
                </div>
              </div>
            )}

            {/* Predictive Latency Pulse Chart */}
            <div className="latency-chart-container" style={{ marginTop: '24px', padding: '16px', backgroundColor: 'rgba(0,0,0,0.2)', borderRadius: '8px', border: '1px solid var(--panel-border)' }}>
              <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '12px', fontSize: '0.85rem' }}>
                <span style={{ color: 'var(--text-muted)', display: 'flex', alignItems: 'center', gap: '6px' }}>
                  <Activity size={14} /> {t('imager.latencyChart')}
                </span>
                <span style={{ color: latencies[latencies.length - 1] > 100 ? 'var(--alert-red)' : 'var(--success-green)', fontFamily: 'monospace' }}>
                  {tFormat('imager.instant', { n: String(latencies.length > 0 ? latencies[latencies.length - 1] : 0) })}
                </span>
              </div>
              <div className="latency-chart" style={{ display: 'flex', alignItems: 'flex-end', height: '60px', gap: '2px', overflow: 'hidden' }}>
                {latencies.map((val, idx) => {
                  const heightPct = Math.min(100, (val / 200) * 100);
                  const isHigh = val > 100;
                  return (
                    <div 
                      key={idx} 
                      style={{ 
                        flex: 1, 
                        height: `${heightPct}%`, 
                        backgroundColor: isHigh ? 'var(--alert-red)' : 'var(--accent-blue)',
                        opacity: 0.8,
                        minHeight: '2px',
                        transition: 'height 0.1s ease-out',
                        borderRadius: '1px 1px 0 0'
                      }} 
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
