import React from 'react'
import './Sidebar.css'
import { LayoutDashboard, Search, FolderSearch, FileSearch, Binary, HardDriveDownload, Activity, ShieldAlert, Database, FileText, Clock, Briefcase, Shield } from 'lucide-react'
import { APP_VERSION } from '../../../shared/app-version'
import { hasValidScanId, isScanDependentPage, isDiskBusyPage, canGenerateReport } from '../../../shared/scan-required'
import { useI18n } from '../../i18n'
import type { ScanState } from '../../../shared/ipc-contract'

interface SidebarProps {
  activePage: string
  activeScanId: number
  /** When set, report nav needs status=complete; results/search/timeline need usable id only. */
  scanState?: ScanState | null
  scanBusy?: boolean
  onNavigate: (page: string) => void
}

type MenuItem = { id: string; labelKey: string; icon: React.ReactNode }

const GROUPS: { titleKey: string; items: MenuItem[] }[] = [
  {
    titleKey: 'nav.groupTitle.recovery',
    items: [
      { id: 'dashboard', labelKey: 'nav.dashboard', icon: <LayoutDashboard size={18} strokeWidth={1.5} /> },
      { id: 'scan', labelKey: 'nav.scan', icon: <Search size={18} strokeWidth={1.5} /> },
      { id: 'results', labelKey: 'nav.results', icon: <FolderSearch size={18} strokeWidth={1.5} /> },
    ],
  },
  {
    titleKey: 'nav.groupTitle.inspect',
    items: [
      { id: 'search', labelKey: 'nav.search', icon: <FileSearch size={18} strokeWidth={1.5} /> },
      { id: 'hex', labelKey: 'nav.hex', icon: <Binary size={18} strokeWidth={1.5} /> },
      { id: 'timeline', labelKey: 'nav.timeline', icon: <Clock size={18} strokeWidth={1.5} /> },
      { id: 'report', labelKey: 'nav.report', icon: <FileText size={18} strokeWidth={1.5} /> },
    ],
  },
  {
    titleKey: 'nav.groupTitle.expert',
    items: [
      { id: 'imager', labelKey: 'nav.imager', icon: <HardDriveDownload size={18} strokeWidth={1.5} /> },
      { id: 'smart', labelKey: 'nav.smart', icon: <Activity size={18} strokeWidth={1.5} /> },
      { id: 'raid', labelKey: 'nav.raid', icon: <Database size={18} strokeWidth={1.5} /> },
      { id: 'case', labelKey: 'nav.case', icon: <Briefcase size={18} strokeWidth={1.5} /> },
      { id: 'shredder', labelKey: 'nav.shredder', icon: <ShieldAlert size={18} strokeWidth={1.5} /> },
    ],
  },
]

function Sidebar({ activePage, activeScanId, scanState, scanBusy, onNavigate }: SidebarProps): React.ReactElement {
  const { t } = useI18n()
  // Results/search/timeline: any hydrated usable scan id (paused or complete).
  const resultsReady = hasValidScanId(activeScanId)
  // Adli rapor: only completed scans (status=1).
  const reportReady = canGenerateReport(activeScanId, scanState ?? undefined)

  return (
    <aside className="sidebar">
      <div className="sidebar-logo">
        <div className="logo-icon" aria-hidden="true">
          <Shield size={16} strokeWidth={2} />
        </div>
        <h1>Byteback</h1>
      </div>
      <nav className="sidebar-nav" aria-label={t('nav.groupTitle.inspect')}>
        {GROUPS.map((group) => (
          <div key={group.titleKey} className="nav-group">
            <h2 className="nav-group-title">{t(group.titleKey)}</h2>
            <ul>
              {group.items.map((item) => {
                const needsScan = isScanDependentPage(item.id)
                const needsIdleDisk = isDiskBusyPage(item.id)
                const scanOk = item.id === 'report' ? reportReady : resultsReady
                const disabled = (needsScan && !scanOk) || (needsIdleDisk && !!scanBusy)
                const isActive = activePage === item.id
                let title: string | undefined
                if (needsScan && !scanOk) {
                  title = item.id === 'report' ? t('nav.reportLockedTitle') : t('nav.scanLockedTitle')
                } else if (needsIdleDisk && scanBusy) {
                  title = t('nav.diskBusyTitle')
                }
                return (
                  <li key={item.id}>
                    <button
                      type="button"
                      data-testid={`nav-${item.id}`}
                      className={`nav-btn ${isActive ? 'active' : ''} ${disabled ? 'nav-btn-disabled' : ''}`}
                      onClick={() => onNavigate(item.id)}
                      disabled={disabled}
                      aria-current={isActive ? 'page' : undefined}
                      title={title}
                    >
                      <span className="nav-icon" aria-hidden="true">{item.icon}</span>
                      <span className="nav-label">{t(item.labelKey)}</span>
                    </button>
                  </li>
                )
              })}
            </ul>
          </div>
        ))}
      </nav>

      <div className="sidebar-footer">
        <div className="pro-badge">Adli</div>
        <div className="version-info">Sürüm v{APP_VERSION}</div>
      </div>
    </aside>
  )
}

export default Sidebar
