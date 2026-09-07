import React, { useEffect, useRef } from 'react'
import { AlertTriangle } from 'lucide-react'
import { useI18n } from '../i18n'

interface ConfirmModalProps {
  open: boolean
  title: string
  body: React.ReactNode
  confirmLabel: string
  cancelLabel?: string
  onConfirm: () => void
  onCancel: () => void
}

/** Modal confirmation for destructive actions (same pattern as SsdTrimModal:
 *  focuses the confirm button, Escape and backdrop click cancel). Replaces
 *  window.confirm so destructive prompts stay in-app and localized. */
function ConfirmModal({ open, title, body, confirmLabel, cancelLabel, onConfirm, onCancel }: ConfirmModalProps): React.ReactElement | null {
  const { t } = useI18n()
  const confirmRef = useRef<HTMLButtonElement>(null)

  useEffect(() => {
    if (!open) return
    confirmRef.current?.focus()
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onCancel()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [open, onCancel])

  if (!open) return null

  return (
    <div
      role="dialog"
      aria-modal="true"
      aria-labelledby="confirm-modal-title"
      data-testid="confirm-modal"
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
          <AlertTriangle size={24} color="var(--warning-yellow)" style={{ flexShrink: 0 }} aria-hidden="true" />
          <div>
            <h3 id="confirm-modal-title" style={{ marginBottom: '8px' }}>{title}</h3>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem', lineHeight: 1.5, margin: 0, whiteSpace: 'pre-wrap' }}>{body}</p>
          </div>
        </div>
        <div style={{ display: 'flex', gap: '12px', justifyContent: 'flex-end' }}>
          <button type="button" className="btn-secondary" onClick={onCancel}>{cancelLabel ?? t('results.confirmCancel')}</button>
          <button ref={confirmRef} type="button" className="btn-primary" data-testid="confirm-modal-accept" onClick={onConfirm}>
            {confirmLabel}
          </button>
        </div>
      </div>
    </div>
  )
}

export default ConfirmModal
