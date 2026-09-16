import React from 'react'
import InlineAlert from './InlineAlert'
import { SCAN_HONESTY_LANES, type ScanHonestyFlags } from '../../shared/scan-honesty'
import { useI18n } from '../i18n'

function HonestyBanners({
  flags,
  loadFailed,
  ns,
  loadFailedTestId,
}: {
  flags: ScanHonestyFlags
  loadFailed: boolean
  ns: 'scan' | 'results'
  loadFailedTestId: string
}): React.ReactElement {
  const { t } = useI18n()
  return (
    <>
      {SCAN_HONESTY_LANES.map((lane) =>
        flags[lane.flag] ? (
          <InlineAlert key={lane.flag} variant="warning" testId={lane.testId}>
            {t(`${ns}.${lane.messageKey}`)}
          </InlineAlert>
        ) : null,
      )}
      {loadFailed && (
        <InlineAlert variant="warning" testId={loadFailedTestId}>
          {t(`${ns}.honestyLoadFailed`)}
        </InlineAlert>
      )}
    </>
  )
}

export default HonestyBanners
