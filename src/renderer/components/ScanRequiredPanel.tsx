import React from 'react'
import { FolderSearch } from 'lucide-react'
import { useI18n } from '../i18n'

interface ScanRequiredPanelProps {
  title?: string
  onGoDashboard?: () => void
}

export default function ScanRequiredPanel({
  title,
  onGoDashboard,
}: ScanRequiredPanelProps): React.ReactElement {
  const { t } = useI18n()
  return (
    <div
      className="scan-required-panel glass-panel"
      role="note"
      style={{
        maxWidth: '560px',
        margin: '48px auto',
        padding: '32px',
        textAlign: 'center',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        gap: '16px',
      }}
    >
      <FolderSearch size={48} color="var(--accent-blue)" aria-hidden="true" />
      <h3 style={{ fontSize: '1.25rem' }}>{title ?? t('needscan.title')}</h3>
      <p style={{ color: 'var(--text-muted)', lineHeight: 1.6 }}>
        {t('needscan.body')}
      </p>
      {onGoDashboard && (
        <button type="button" className="btn-primary" onClick={onGoDashboard}>
          {t('needscan.goDashboard')}
        </button>
      )}
    </div>
  )
}
