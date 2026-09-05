import React, { useState } from 'react';
import './ShredderView.css';
import { AlertTriangle, ShieldAlert, CheckCircle, FileWarning } from 'lucide-react';
import { useI18n, tFormat } from '../../i18n';

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
    window.api.listDrives().then((list) => {
      setDrives(list)
      setSsdWarning(list.some((d: { type?: string }) => d.type === 'SSD'))
    }).catch((e: unknown) => {
      console.error('listDrives failed', e)
      setWipeError(t('shred.drivesFailed'))
    })
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
    <div className="shredder-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%', maxWidth: '800px', margin: '0 auto' }}>
      <div className="shredder-header glass-panel" style={{ padding: '24px', display: 'flex', gap: '16px', alignItems: 'center', borderLeft: '4px solid var(--alert-red)' }}>
        <div style={{ background: 'rgba(239, 68, 68, 0.1)', padding: '16px', borderRadius: '12px' }}>
          <ShieldAlert size={32} color="var(--alert-red)" />
        </div>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px', color: 'var(--alert-red)' }}>{t('title.shredder')}</h2>
          <p style={{ color: 'var(--text-muted)' }}>{t('shred.subtitle')}</p>
        </div>
      </div>

      <div className="shredder-content" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
        <div className="shredder-warning glass-panel" style={{ padding: '20px', background: 'rgba(239, 68, 68, 0.05)', display: 'flex', gap: '16px', alignItems: 'flex-start' }}>
          <AlertTriangle size={24} color="var(--warning-yellow)" style={{ flexShrink: 0, marginTop: '2px' }} />
          <div>
            <h4 style={{ color: 'var(--warning-yellow)', marginBottom: '8px', fontSize: '1.1rem' }}>{t('shred.limitsTitle')}</h4>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem', lineHeight: 1.6 }}>
              {t('shred.limitsBody')}
              {ssdWarning ? t('shred.ssdSeen') : '.'} {t('shred.physicalIrreversible')}
            </p>
          </div>
        </div>

        <div className="shredder-card glass-panel" style={{ padding: '32px', display: 'flex', flexDirection: 'column', gap: '16px' }}>
          {status === 'idle' && (
            <>
              <button
                className="btn-danger shred-btn"
                onClick={() => void handleFreeSpaceWipe()}
                style={{ padding: '16px', fontSize: '1.1rem', fontWeight: 600, display: 'flex', justifyContent: 'center', alignItems: 'center', gap: '12px' }}
              >
                <ShieldAlert size={20} /> {t('shred.freeSpaceBtn')}
              </button>
              <button
                type="button"
                className="btn-secondary"
                onClick={() => void handleFileWipe()}
                style={{ padding: '12px', display: 'flex', justifyContent: 'center', alignItems: 'center', gap: '8px' }}
              >
                <FileWarning size={18} /> {t('shred.fileBtn')}
              </button>
              <label style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>{t('shred.physicalLabel')}</label>
              <select
                aria-label={t('shred.driveAria')}
                value={wipeIndex}
                onChange={(e) => setWipeIndex(Number(e.target.value))}
                style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
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
                style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
              />
              <input
                aria-label={t('shred.imhaAria')}
                placeholder={t('shred.imhaPlaceholder')}
                value={confirmPhrase}
                onChange={(e) => setConfirmPhrase(e.target.value.toUpperCase())}
                style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
              />
              {selectedDrive && (
                <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>
                  {tFormat('shred.target', { model: selectedDrive.model || t('dash.diskFallback'), serial: selectedDrive.serial || t('shred.noSerial'), type: selectedDrive.type || t('scan.unknown') })}
                </p>
              )}
              <button
                type="button"
                className="btn-danger"
                disabled={!typedSerial.trim() || confirmPhrase !== 'IMHA' || drives.length === 0}
                onClick={() => void handlePhysicalWipe()}
                style={{ padding: '12px' }}
              >
                {t('shred.physicalBtn')}
              </button>
              {wipeError && (
                <p role="alert" style={{ color: 'var(--alert-red)', fontSize: '0.85rem' }}>{wipeError}</p>
              )}
            </>
          )}

          {status === 'shredding' && (
            <div className="shred-progress glass-panel" role="status" aria-busy="true" style={{ padding: '24px', border: '1px solid rgba(239, 68, 68, 0.2)' }}>
              <p style={{ color: 'var(--alert-red)' }}>{t('shred.progressNote')}</p>
            </div>
          )}

          {status === 'done' && (
            <div className="shred-success glass-panel" style={{ padding: '24px', textAlign: 'center' }}>
              <CheckCircle size={48} color="var(--success-green)" style={{ margin: '0 auto 16px' }} />
              <h3 style={{ color: 'var(--success-green)', marginBottom: '8px' }}>{t('shred.doneTitle')}</h3>
              <p style={{ color: 'var(--text-muted)', marginBottom: '16px' }}>{t('shred.doneBody')}</p>
              <button className="btn-secondary" onClick={() => setStatus('idle')}>{t('shred.newOp')}</button>
            </div>
          )}

          {status === 'failed' && (
            <div className="glass-panel" role="alert" style={{ padding: '24px', textAlign: 'center' }}>
              <h3 style={{ marginBottom: '8px' }}>{t('shred.failedTitle')}</h3>
              <p style={{ color: 'var(--text-muted)', marginBottom: '16px' }}>
                {wipeError ?? t('shred.failedBody')}
              </p>
              <button className="btn-secondary" onClick={() => { setStatus('idle'); setWipeError(null) }}>{t('shred.back')}</button>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}

export default ShredderView
