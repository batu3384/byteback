import React from 'react'
import './Sidebar.css'
import { LayoutDashboard, Search, FolderSearch, FileSearch, Binary, HardDriveDownload, Activity, ShieldAlert, Database, FileText, Clock, Briefcase, Shield } from 'lucide-react'
import { APP_VERSION } from '../../../shared/app-version'
import { hasValidScanId, isScanDependentPage, isDiskBusyPage } from '../../../shared/scan-required'

interface SidebarProps {
  activePage: string
  activeScanId: number
  scanBusy?: boolean
  onNavigate: (page: string) => void
}

type MenuItem = { id: string; label: string; icon: React.ReactNode }

const GROUPS: { title: string; items: MenuItem[] }[] = [
  {
    title: 'Kurtarma',
    items: [
      { id: 'dashboard', label: 'Ana Ekran', icon: <LayoutDashboard size={18} strokeWidth={1.5} /> },
      { id: 'scan', label: 'Aktif Tarama', icon: <Search size={18} strokeWidth={1.5} /> },
      { id: 'results', label: 'Sonuçlar', icon: <FolderSearch size={18} strokeWidth={1.5} /> },
    ],
  },
  {
    title: 'İnceleme',
    items: [
      { id: 'search', label: 'Kelime Arama', icon: <FileSearch size={18} strokeWidth={1.5} /> },
      { id: 'hex', label: 'Hex İnceleyici', icon: <Binary size={18} strokeWidth={1.5} /> },
      { id: 'timeline', label: 'Zaman Çizelgesi', icon: <Clock size={18} strokeWidth={1.5} /> },
      { id: 'report', label: 'Adli Rapor', icon: <FileText size={18} strokeWidth={1.5} /> },
    ],
  },
  {
    title: 'Disk / uzman',
    items: [
      { id: 'imager', label: 'İmaj (RAW / E01)', icon: <HardDriveDownload size={18} strokeWidth={1.5} /> },
      { id: 'smart', label: 'S.M.A.R.T.', icon: <Activity size={18} strokeWidth={1.5} /> },
      { id: 'raid', label: 'Sanal RAID', icon: <Database size={18} strokeWidth={1.5} /> },
      { id: 'case', label: 'Dava / NSRL', icon: <Briefcase size={18} strokeWidth={1.5} /> },
      { id: 'shredder', label: 'Veri Yok Edici', icon: <ShieldAlert size={18} strokeWidth={1.5} /> },
    ],
  },
]

function Sidebar({ activePage, activeScanId, scanBusy, onNavigate }: SidebarProps): React.ReactElement {
  const scanReady = hasValidScanId(activeScanId)

  return (
    <aside className="sidebar">
      <div className="sidebar-logo">
        <div className="logo-icon" aria-hidden="true">
          <Shield size={16} strokeWidth={2} />
        </div>
        <h1>Byteback</h1>
      </div>
      <nav className="sidebar-nav" aria-label="Ana menü">
        {GROUPS.map((group) => (
          <div key={group.title} className="nav-group">
            <h2 className="nav-group-title">{group.title}</h2>
            <ul>
              {group.items.map((item) => {
                const needsScan = isScanDependentPage(item.id)
                const needsIdleDisk = isDiskBusyPage(item.id)
                const disabled = (needsScan && !scanReady) || (needsIdleDisk && !!scanBusy)
                const isActive = activePage === item.id
                let title: string | undefined
                if (needsScan && !scanReady) title = 'Önce bir taramayı tamamlayın'
                else if (needsIdleDisk && scanBusy) title = 'Hex, imaj ve imha tarama bitene kadar kapalı'
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
                      <span className="nav-label">{item.label}</span>
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
