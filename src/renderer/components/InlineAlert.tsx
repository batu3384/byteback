import React from 'react'
import { AlertCircle, CheckCircle, AlertTriangle, X } from 'lucide-react'
import { useI18n } from '../i18n'

type AlertVariant = 'error' | 'warning' | 'success' | 'info'

interface InlineAlertProps {
  variant?: AlertVariant
  title?: string
  children: React.ReactNode
  onDismiss?: () => void
  role?: 'alert' | 'status'
  testId?: string
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
  testId,
}: InlineAlertProps): React.ReactElement {
  const { t } = useI18n()
  return (
    <div
      role={role}
      className={`inline-alert glass-panel is-${variant}`}
      data-testid={testId}
    >
      <span className="inline-alert-ico">
        <AlertIcon variant={variant} />
      </span>
      <div className="inline-alert-copy">
        {title && <strong>{title}</strong>}
        <div className="inline-alert-body">{children}</div>
      </div>
      {onDismiss && (
        <button
          type="button"
          className="icon-btn"
          aria-label={t('common.close')}
          onClick={onDismiss}
        >
          <X size={16} />
        </button>
      )}
    </div>
  )
}
