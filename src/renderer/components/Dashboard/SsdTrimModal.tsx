import React, { useEffect, useRef } from 'react'
import { AlertTriangle } from 'lucide-react'
import type { ScanProfile } from '../../../shared/scan-profiles'
import { useI18n } from '../../i18n'

interface SsdTrimModalProps {
  open: boolean
  scanType: ScanProfile
  onConfirm: () => void
  onCancel: () => void
}

function SsdTrimModal({ open, scanType, onConfirm, onCancel }: SsdTrimModalProps): React.ReactElement | null {
  const { t } = useI18n()
  const confirmRef = useRef<HTMLButtonElement>(null)
  // Callers pass inline handlers (Dashboard onCancel={() => ...}). The
  // open/focus effect must key on `open` only — re-running it on every parent
  // re-render (drive polling) would yank focus back to the confirm button.
  // Same pattern as ConfirmModal.
  const onCancelRef = useRef(onCancel)
  useEffect(() => {
    onCancelRef.current = onCancel
  })

  useEffect(() => {
    if (!open) return
    confirmRef.current?.focus()
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onCancelRef.current()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [open])

  if (!open) return null

  return (
    <div
      role="dialog"
      aria-modal="true"
      aria-labelledby="ssd-trim-title"
      data-testid="ssd-trim-modal"
      style={{
        position: 'fixed',
        inset: 0,
        zIndex: 1000,
        background: 'rgba(0,0,0,0.65)',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        padding: '24px',
      }}
      onClick={onCancel}
    >
      <div
        className="glass-panel"
        style={{ maxWidth: '480px', padding: '24px', display: 'flex', flexDirection: 'column', gap: '16px' }}
        onClick={(e) => e.stopPropagation()}
      >
        <div style={{ display: 'flex', gap: '12px', alignItems: 'flex-start' }}>
          <AlertTriangle size={28} color="var(--warning-yellow)" style={{ flexShrink: 0 }} aria-hidden="true" />
          <div>
            <h3 id="ssd-trim-title" style={{ marginBottom: '8px' }}>{t('ssd.title')}</h3>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem', lineHeight: 1.5, margin: 0 }}>
              {t('ssd.body')}
            </p>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem', marginTop: '12px', marginBottom: 0 }}>
              {t('ssd.mode')} <strong>{t(`profile.${scanType}.label`)}</strong> — {t(`profile.${scanType}.detail`)}
            </p>
            {(scanType === 'full_carve' || scanType === 'carve_only') && (
              <p style={{ color: 'var(--warning-yellow)', fontSize: '0.85rem', marginTop: '8px', marginBottom: 0 }}>
                {scanType === 'carve_only'
                  ? t('ssd.carveOnlyWarning')
                  : t('ssd.fullCarveWarning')}
              </p>
            )}
          </div>
        </div>
        <div style={{ display: 'flex', gap: '12px', justifyContent: 'flex-end' }}>
          <button type="button" className="btn-secondary" onClick={onCancel}>{t('ssd.cancel')}</button>
          <button ref={confirmRef} type="button" className="btn-primary" data-testid="ssd-trim-confirm" onClick={onConfirm}>
            {t('ssd.scanAnyway')}
          </button>
        </div>
      </div>
    </div>
  )
}

export default SsdTrimModal
