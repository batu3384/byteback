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

// Open-modal registry: with two stacked ConfirmModals every instance hears the
// same window keydown, so Escape must be acted on by the topmost registered
// modal only (the underlying one stays open). Single-modal behavior — the only
// usage today — is unchanged: one registration, always on top.
let modalSeq = 0
const modalStack: number[] = []

/** Modal confirmation for destructive actions (same pattern as SsdTrimModal:
 *  focuses the confirm button, Escape and backdrop click cancel). Replaces
 *  window.confirm so destructive prompts stay in-app and localized. */
function ConfirmModal({ open, title, body, confirmLabel, cancelLabel, onConfirm, onCancel }: ConfirmModalProps): React.ReactElement | null {
  const { t } = useI18n()
  const confirmRef = useRef<HTMLButtonElement>(null)
  const panelRef = useRef<HTMLDivElement>(null)
  const restoreFocusRef = useRef<HTMLElement | null>(null)
  // Callers pass inline handlers (e.g. Dashboard onCancel={() => ...}). The
  // open/focus/teardown effect must key on `open` only — re-running it on
  // every parent re-render would yank focus back to the confirm button and
  // pollute the restore target while the dialog is still open.
  const onCancelRef = useRef(onCancel)
  useEffect(() => {
    onCancelRef.current = onCancel
  })

  useEffect(() => {
    if (!open) return
    // Register on the open-modal stack; Escape is only acted on while this
    // instance is the topmost entry (stacked-modals case).
    const modalId = ++modalSeq
    modalStack.push(modalId)
    // Focus trap: Tab/Shift+Tab cycle inside the dialog; the trigger gets
    // focus back when the dialog closes.
    restoreFocusRef.current = document.activeElement as HTMLElement | null
    confirmRef.current?.focus()
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        if (modalStack[modalStack.length - 1] === modalId) onCancelRef.current()
        return
      }
      if (e.key !== 'Tab' || !panelRef.current) return
      const focusables = panelRef.current.querySelectorAll<HTMLElement>(
        'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])',
      )
      if (focusables.length === 0) return
      const first = focusables[0]
      const last = focusables[focusables.length - 1]
      const active = document.activeElement
      if (!panelRef.current.contains(active)) {
        e.preventDefault()
        first.focus()
      } else if (e.shiftKey && active === first) {
        e.preventDefault()
        last.focus()
      } else if (!e.shiftKey && active === last) {
        e.preventDefault()
        first.focus()
      }
    }
    window.addEventListener('keydown', onKey)
    return () => {
      window.removeEventListener('keydown', onKey)
      const at = modalStack.indexOf(modalId)
      if (at !== -1) modalStack.splice(at, 1)
      restoreFocusRef.current?.focus?.()
    }
  }, [open])

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
        ref={panelRef}
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
