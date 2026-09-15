import React from 'react'
import ConfirmModal from '../ConfirmModal'
import type { ScanProfile } from '../../../shared/scan-profiles'
import { useI18n } from '../../i18n'

interface SsdTrimModalProps {
  open: boolean
  scanType: ScanProfile
  unknownMedia?: boolean
  onConfirm: () => void
  onCancel: () => void
}

function SsdTrimModal({ open, scanType, unknownMedia, onConfirm, onCancel }: SsdTrimModalProps): React.ReactElement {
  const { t } = useI18n()
  return (
    <ConfirmModal
      open={open}
      title={t(unknownMedia ? 'ssd.titleUnread' : 'ssd.title')}
      confirmLabel={t('ssd.scanAnyway')}
      cancelLabel={t('ssd.cancel')}
      onConfirm={onConfirm}
      onCancel={onCancel}
      dialogTestId="ssd-trim-modal"
      confirmTestId="ssd-trim-confirm"
      titleId="ssd-trim-title"
      body={(
        <>
          <p>{t(unknownMedia ? 'ssd.bodyUnread' : 'ssd.body')}</p>
          <p className="ssd-mode">
            {t('ssd.mode')} <strong>{t(`profile.${scanType}.label`)}</strong> — {t(`profile.${scanType}.detail`)}
          </p>
          {(scanType === 'full_carve' || scanType === 'carve_only') && (
            <p className="ssd-carve-warn">
              {scanType === 'carve_only'
                ? t('ssd.carveOnlyWarning')
                : t('ssd.fullCarveWarning')}
            </p>
          )}
        </>
      )}
    />
  )
}

export default SsdTrimModal
