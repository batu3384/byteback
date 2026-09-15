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
    >
      <FolderSearch size={48} color="var(--accent-blue)" aria-hidden="true" />
      <h3>{title ?? t('needscan.title')}</h3>
      <p>
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
