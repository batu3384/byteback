import React from 'react'
import './StatusBar.css'
import { useI18n, tFormat } from '../../i18n'
import { APP_VERSION } from '../../../shared/app-version'

interface StatusBarProps {
  caseNumber?: string
  scanBusy?: boolean
  scanPercent?: number
  driveIndex?: number | null
}

function StatusBar({ caseNumber, scanBusy, scanPercent, driveIndex }: StatusBarProps): React.ReactElement {
  const { t } = useI18n()
  const caseLabel = caseNumber?.trim()
    ? tFormat('chrome.statusCase', { n: caseNumber.trim() })
    : t('chrome.statusNoCase')
  const scanLabel = scanBusy
    ? tFormat('chrome.statusScan', { n: String(scanPercent ?? 0) })
    : t('chrome.statusIdle')
  const driveLabel = driveIndex != null && driveIndex >= 0
    ? tFormat('chrome.statusDrive', { n: String(driveIndex) })
    : t('chrome.statusNoDrive')

  return (
    <footer className="app-statusbar" role="status" data-testid="examiner-statusbar">
      <span>{caseLabel}</span>
      <span className="statusbar-sep" aria-hidden="true">·</span>
      <span>{scanLabel}</span>
      <span className="statusbar-sep" aria-hidden="true">·</span>
      <span>{driveLabel}</span>
      <span className="statusbar-spacer" />
      <span>{tFormat('sidebar.version', { v: APP_VERSION })}</span>
    </footer>
  )
}

export default StatusBar
