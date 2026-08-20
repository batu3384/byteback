import React, { useEffect, useRef } from 'react'
import { AlertTriangle } from 'lucide-react'
import type { ScanProfile } from '../../../shared/scan-profiles'
import { SCAN_PROFILES } from '../../../shared/scan-profiles'

interface SsdTrimModalProps {
  open: boolean
  scanType: ScanProfile
  onConfirm: () => void
  onCancel: () => void
}

function SsdTrimModal({ open, scanType, onConfirm, onCancel }: SsdTrimModalProps): React.ReactElement | null {
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

  const profile = SCAN_PROFILES[scanType]

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
            <h3 id="ssd-trim-title" style={{ marginBottom: '8px' }}>SSD / TRIM uyarısı</h3>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem', lineHeight: 1.5, margin: 0 }}>
              Bu sürücü SSD olarak tespit edildi. TRIM, silinen veriyi fiziksel olarak temizleyebilir —
              kurtarma başarı oranı düşük olabilir. Mümkünse imaj alın veya taramayı hemen başlatın.
            </p>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem', marginTop: '12px', marginBottom: 0 }}>
              Seçilen mod: <strong>{profile.label}</strong> — {profile.detail}
            </p>
            {scanType === 'full_carve' && (
              <p style={{ color: 'var(--warning-yellow)', fontSize: '0.85rem', marginTop: '8px', marginBottom: 0 }}>
                Tam disk carve tüm sektörleri tarar; SSD üzerinde çok uzun sürebilir.
              </p>
            )}
          </div>
        </div>
        <div style={{ display: 'flex', gap: '12px', justifyContent: 'flex-end' }}>
          <button type="button" className="btn-secondary" onClick={onCancel}>İptal</button>
          <button ref={confirmRef} type="button" className="btn-primary" data-testid="ssd-trim-confirm" onClick={onConfirm}>
            Yine de tara
          </button>
        </div>
      </div>
    </div>
  )
}

export default SsdTrimModal
