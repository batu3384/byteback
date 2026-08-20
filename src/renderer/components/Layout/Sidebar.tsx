import React from 'react'
import './Sidebar.css'
import { LayoutDashboard, Search, FolderSearch, FileSearch, Binary, HardDriveDownload, Activity, ShieldAlert, Database, FileText, Clock, Briefcase, Shield } from 'lucide-react'
import { APP_VERSION } from '../../../shared/app-version'
import { hasValidScanId, isScanDependentPage } from '../../../shared/scan-required'

interface SidebarProps {
  activePage: string
  activeScanId: number
  onNavigate: (page: string) => void
}

function Sidebar({ activePage, activeScanId, onNavigate }: SidebarProps): React.ReactElement {
  const scanReady = hasValidScanId(activeScanId)

  const menuItems = [
    { id: 'dashboard', label: 'Ana Ekran', icon: <LayoutDashboard size={20} strokeWidth={1.5} /> },
    { id: 'scan', label: 'Aktif Tarama', icon: <Search size={20} strokeWidth={1.5} /> },
    { id: 'results', label: 'Kurtarma Sonuçları', icon: <FolderSearch size={20} strokeWidth={1.5} /> },
    { id: 'search', label: 'Kelime Arama', icon: <FileSearch size={20} strokeWidth={1.5} /> },
    { id: 'hex', label: 'Hex İnceleyici', icon: <Binary size={20} strokeWidth={1.5} /> },
    { id: 'imager', label: 'İmaj (RAW / E01)', icon: <HardDriveDownload size={20} strokeWidth={1.5} /> },
    { id: 'smart', label: 'S.M.A.R.T. Durumu', icon: <Activity size={20} strokeWidth={1.5} /> },
    { id: 'shredder', label: 'Veri Yok Edici', icon: <ShieldAlert size={20} strokeWidth={1.5} /> },
    { id: 'raid', label: 'Sanal RAID Oluştur', icon: <Database size={20} strokeWidth={1.5} /> },
    { id: 'timeline', label: 'Olay Zaman Çizelgesi', icon: <Clock size={20} strokeWidth={1.5} /> },
    { id: 'report', label: 'Adli Rapor (PDF)', icon: <FileText size={20} strokeWidth={1.5} /> },
    { id: 'case', label: 'Dava / NSRL', icon: <Briefcase size={20} strokeWidth={1.5} /> },
  ]

  return (
    <aside className="sidebar">
      <div className="sidebar-logo">
        <div className="logo-icon" aria-hidden="true">
          <Shield size={16} strokeWidth={2} />
        </div>
        <h1>Byteback</h1>
      </div>
      <nav className="sidebar-nav" aria-label="Ana menü">
        <ul>
          {menuItems.map((item) => {
            const needsScan = isScanDependentPage(item.id)
            const disabled = needsScan && !scanReady
            const isActive = activePage === item.id
            return (
              <li key={item.id}>
                <button
                  type="button"
                  className={`nav-btn ${isActive ? 'active' : ''} ${disabled ? 'nav-btn-disabled' : ''}`}
                  onClick={() => onNavigate(item.id)}
                  disabled={disabled}
                  aria-current={isActive ? 'page' : undefined}
                  title={disabled ? 'Önce bir taramayı tamamlayın' : undefined}
                >
                  <span className="nav-icon" aria-hidden="true">{item.icon}</span>
                  <span className="nav-label">{item.label}</span>
                </button>
              </li>
            )
          })}
        </ul>
      </nav>

      <div className="sidebar-footer">
        <div className="pro-badge">Adli</div>
        <div className="version-info">Sürüm v{APP_VERSION}</div>
      </div>
    </aside>
  )
}

export default Sidebar
