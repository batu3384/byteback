import React from 'react'
import './Sidebar.css'
import { LayoutDashboard, Search, FolderSearch, FileSearch, Binary, HardDriveDownload, Activity, ShieldAlert, Database, FileText } from 'lucide-react'

interface SidebarProps {
  activePage: string
  onNavigate: (page: any) => void
}

function Sidebar({ activePage, onNavigate }: SidebarProps): React.ReactElement {
  const menuItems = [
    { id: 'dashboard', label: 'Ana Ekran', icon: <LayoutDashboard size={20} strokeWidth={1.5} /> },
    { id: 'scan', label: 'Aktif Tarama', icon: <Search size={20} strokeWidth={1.5} /> },
    { id: 'results', label: 'Kurtarma Sonuçları', icon: <FolderSearch size={20} strokeWidth={1.5} /> },
    { id: 'search', label: 'Kelime Arama', icon: <FileSearch size={20} strokeWidth={1.5} /> },
    { id: 'hex', label: 'Hex İnceleyici', icon: <Binary size={20} strokeWidth={1.5} /> },
    { id: 'imager', label: 'İmaj Alma (DD)', icon: <HardDriveDownload size={20} strokeWidth={1.5} /> },
    { id: 'smart', label: 'S.M.A.R.T. Durumu', icon: <Activity size={20} strokeWidth={1.5} /> },
    { id: 'shredder', label: 'Veri Yok Edici', icon: <ShieldAlert size={20} strokeWidth={1.5} /> },
    { id: 'raid', label: 'Sanal RAID Oluştur', icon: <Database size={20} strokeWidth={1.5} /> },
    { id: 'report', label: 'Adli Rapor (PDF)', icon: <FileText size={20} strokeWidth={1.5} /> },
  ]

  return (
    <aside className="sidebar">
      <div className="sidebar-logo">
        <div className="logo-icon">🐺</div>
        <h1>Wolf Recovery</h1>
      </div>
      <nav className="sidebar-nav">
        <ul>
          {menuItems.map((item) => (
            <li key={item.id}>
              <button
                className={`nav-btn ${activePage === item.id ? 'active' : ''}`}
                onClick={() => onNavigate(item.id)}
              >
                <span className="nav-icon">{item.icon}</span>
                <span className="nav-label">{item.label}</span>
              </button>
            </li>
          ))}
        </ul>
      </nav>
      
      <div className="sidebar-footer">
        <div className="pro-badge">PRO MAX</div>
        <div className="version-info">Adli Bilişim Sürümü (v1.0.0)</div>
      </div>
    </aside>
  )
}

export default Sidebar
