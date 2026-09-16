import React, { useState } from 'react';
import './ShredderView.css';
import { AlertTriangle, ShieldAlert, CheckCircle, FileWarning } from 'lucide-react';
import { useI18n, tFormat } from '../../i18n';
import InlineAlert from '../InlineAlert';

const ShredderView: React.FC = () => {
  const { t } = useI18n()
  const [status, setStatus] = useState<'idle' | 'shredding' | 'done' | 'failed'>('idle')
  const [ssdWarning, setSsdWarning] = useState(false)
  const [drives, setDrives] = useState<{ index: number; model?: string; serial?: string; type?: string; sizeBytes?: number }[]>([])
  const [wipeIndex, setWipeIndex] = useState(0)
  const [typedSerial, setTypedSerial] = useState('')
  const [confirmPhrase, setConfirmPhrase] = useState('')
  const [wipeError, setWipeError] = useState<string | null>(null)

  React.useEffect(() => {
    if (!window.api?.listDrives) return
    let alive = true
    window.api.listDrives().then((list) => {
      if (!alive) return
      setDrives(list)
      setSsdWarning(list.some((d: { type?: string }) => d.type === 'SSD'))
    }).catch((e: unknown) => {
      console.error('listDrives failed', e)
      if (alive) setWipeError(t('shred.drivesFailed'))
    })
    return () => { alive = false }
  }, [])

  const handleFreeSpaceWipe = async () => {
    if (!window.api?.pickAndWipeFreeSpace) return
    setWipeError(null)
    setStatus('shredding')
    try {
      const res = await window.api.pickAndWipeFreeSpace()
      if (res.ok) setStatus('done')
      else {
        setWipeError(res.error ?? t('shred.freeSpaceFailed'))
        setStatus('failed')
      }
    } catch (e: unknown) {
      setWipeError(e instanceof Error ? e.message : t('shred.freeSpaceError'))
      setStatus('failed')
    }
  }

  const handleFileWipe = async () => {
    if (!window.api?.pickAndWipeFile) return
    setWipeError(null)
    setStatus('shredding')
    try {
      const res = await window.api.pickAndWipeFile()
      if (res.ok) setStatus('done')
      else {
        setWipeError(res.error ?? t('shred.fileFailed'))
        setStatus('failed')
      }
    } catch (e: unknown) {
      setWipeError(e instanceof Error ? e.message : t('shred.fileError'))
      setStatus('failed')
    }
  }

  const handlePhysicalWipe = async () => {
    if (!window.api?.wipePhysicalDrive || !typedSerial.trim() || confirmPhrase !== 'IMHA') return
    setWipeError(null)
    setStatus('shredding')
    try {
      const res = await window.api.wipePhysicalDrive(wipeIndex, typedSerial, confirmPhrase)
      if (res.ok) {
        setStatus('done')
      } else {
        setWipeError(res.error ?? t('shred.diskFailed'))
        setStatus('failed')
      }
    } catch (e: unknown) {
      setWipeError(e instanceof Error ? e.message : t('shred.diskError'))
      setStatus('failed')
    }
  }

  const selectedDrive = drives.find((d) => d.index === wipeIndex)

  return (
    <div className="shredder-view">
      <div className="shredder-header glass-panel">
        <div className="examiner-icon danger" aria-hidden="true">
          <ShieldAlert size={20} color="var(--alert-red)" />
        </div>
        <div>
          <h2>{t('title.shredder')}</h2>
          <p>{t('shred.subtitle')}</p>
        </div>
      </div>

      <div className="shredder-content">
        <div className="shredder-warning glass-panel">
          <AlertTriangle size={24} color="var(--warning-yellow)" className="warn-ico" />
          <div>
            <h4>{t('shred.limitsTitle')}</h4>
            <p>
              {t('shred.limitsBody')}
              {ssdWarning ? t('shred.ssdSeen') : '.'} {t('shred.physicalIrreversible')}
            </p>
          </div>
        </div>

        <div className="shredder-card glass-panel">
          {status === 'idle' && (
            <>
              <button
                type="button"
                className="btn-danger btn-danger-block"
                data-testid="shred-free-space"
                onClick={() => void handleFreeSpaceWipe()}
              >
                <ShieldAlert size={20} /> {t('shred.freeSpaceBtn')}
              </button>
              <button
                type="button"
                className="btn-secondary shred-file-btn"
                data-testid="shred-file"
                onClick={() => void handleFileWipe()}
              >
                <FileWarning size={18} /> {t('shred.fileBtn')}
              </button>
              <label className="shred-label">{t('shred.physicalLabel')}</label>
              <select
                aria-label={t('shred.driveAria')}
                value={wipeIndex}
                onChange={(e) => setWipeIndex(Number(e.target.value))}
                className="drive-select"
                data-testid="shred-drive-select"
              >
                {drives.map((d) => (
                  <option key={d.index} value={d.index}>
                    {d.index}: {d.model || t('dash.diskFallback')} ({d.serial || t('shred.noSerial')})
                  </option>
                ))}
              </select>
              <input
                aria-label={t('shred.serialAria')}
                placeholder={t('shred.serialPlaceholder')}
                value={typedSerial}
                onChange={(e) => setTypedSerial(e.target.value)}
                className="shred-input"
                data-testid="shred-serial"
              />
              <input
                aria-label={t('shred.imhaAria')}
                placeholder={t('shred.imhaPlaceholder')}
                value={confirmPhrase}
                onChange={(e) => setConfirmPhrase(e.target.value.toUpperCase())}
                className="shred-input"
                data-testid="shred-imha"
              />
              {selectedDrive && (
                <p className="shred-label">
                  {tFormat('shred.target', { model: selectedDrive.model || t('dash.diskFallback'), serial: selectedDrive.serial || t('shred.noSerial'), type: selectedDrive.type || t('scan.unknown') })}
                </p>
              )}
              <button
                type="button"
                className="btn-danger btn-danger-block"
                data-testid="shred-physical"
                disabled={!typedSerial.trim() || confirmPhrase !== 'IMHA' || drives.length === 0}
                onClick={() => void handlePhysicalWipe()}
              >
                {t('shred.physicalBtn')}
              </button>
              {wipeError && (
                <InlineAlert variant="error" testId="shred-error">{wipeError}</InlineAlert>
              )}
            </>
          )}

          {status === 'shredding' && (
            <div className="shred-progress glass-panel" role="status" aria-busy="true" data-testid="shred-progress">
              <p>{t('shred.progressNote')}</p>
            </div>
          )}

          {status === 'done' && (
            <div className="examiner-empty" role="status" data-testid="shred-done">
              <div className="examiner-icon ok" aria-hidden="true">
                <CheckCircle size={28} color="var(--success-green)" />
              </div>
              <h3>{t('shred.doneTitle')}</h3>
              <p>{t('shred.doneBody')}</p>
              <button type="button" className="btn-secondary" onClick={() => setStatus('idle')}>{t('shred.newOp')}</button>
            </div>
          )}

          {status === 'failed' && (
            <div className="examiner-empty is-error" data-testid="shred-failed">
              <InlineAlert variant="error" title={t('shred.failedTitle')}>
                {wipeError ?? t('shred.failedBody')}
              </InlineAlert>
              <button type="button" className="btn-secondary" onClick={() => { setStatus('idle'); setWipeError(null) }}>{t('shred.back')}</button>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}

export default ShredderView
