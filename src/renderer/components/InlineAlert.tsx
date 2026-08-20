import React from 'react'
import { AlertCircle, CheckCircle, AlertTriangle, X } from 'lucide-react'

type AlertVariant = 'error' | 'warning' | 'success' | 'info'

interface InlineAlertProps {
  variant?: AlertVariant
  title?: string
  children: React.ReactNode
  onDismiss?: () => void
  role?: 'alert' | 'status'
}

const colors: Record<AlertVariant, { border: string; bg: string; icon: string }> = {
  error: { border: 'var(--alert-red)', bg: 'rgba(239, 68, 68, 0.12)', icon: 'var(--alert-red)' },
  warning: { border: 'var(--warning-yellow)', bg: 'rgba(245, 158, 11, 0.12)', icon: 'var(--warning-yellow)' },
  success: { border: 'var(--success-green)', bg: 'rgba(16, 185, 129, 0.12)', icon: 'var(--success-green)' },
  info: { border: 'var(--accent-blue)', bg: 'rgba(59, 130, 246, 0.12)', icon: 'var(--accent-blue)' },
}

function AlertIcon({ variant }: { variant: AlertVariant }) {
  const size = 18
  if (variant === 'success') return <CheckCircle size={size} aria-hidden="true" />
  if (variant === 'warning') return <AlertTriangle size={size} aria-hidden="true" />
  return <AlertCircle size={size} aria-hidden="true" />
}

export default function InlineAlert({
  variant = 'info',
  title,
  children,
  onDismiss,
  role = variant === 'error' ? 'alert' : 'status',
}: InlineAlertProps): React.ReactElement {
  const c = colors[variant]
  return (
    <div
      role={role}
      className="inline-alert glass-panel"
      style={{
        padding: '12px 16px',
        borderLeft: `3px solid ${c.border}`,
        background: c.bg,
        display: 'flex',
        gap: '12px',
        alignItems: 'flex-start',
      }}
    >
      <span style={{ color: c.icon, flexShrink: 0, marginTop: '2px' }}>
        <AlertIcon variant={variant} />
      </span>
      <div style={{ flex: 1, minWidth: 0 }}>
        {title && <strong style={{ display: 'block', marginBottom: '4px' }}>{title}</strong>}
        <div style={{ color: 'var(--text-main)', fontSize: '0.92rem', lineHeight: 1.5 }}>{children}</div>
      </div>
      {onDismiss && (
        <button
          type="button"
          className="icon-btn"
          aria-label="Kapat"
          onClick={onDismiss}
          style={{ flexShrink: 0, padding: '4px' }}
        >
          <X size={16} />
        </button>
      )}
    </div>
  )
}
