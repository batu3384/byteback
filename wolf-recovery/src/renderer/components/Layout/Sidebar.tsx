import React from 'react'
import './Sidebar.css'

interface SidebarProps {
  activePage: string
  onNavigate: (page: any) => void
}

function Sidebar({ activePage, onNavigate }: SidebarProps): React.ReactElement {
  const menuItems = [
    { id: 'dashboard', label: 'Ana Ekran', icon: '📊' },
    { id: 'scan', label: 'Aktif Tarama', icon: '🔍' },
    { id: 'results', label: 'Kurtarma Sonuçları', icon: '📂' },
    { id: 'search', label: 'Kelime Arama', icon: '🔎' },
    { id: 'hex', label: 'Hex İnceleyici', icon: '💻' },
    { id: 'imager', label: 'Disk İmajı', icon: '💿' },
    { id: 'smart', label: 'SMART Durumu', icon: '🩺' },
    { id: 'shredder', label: 'Data Shredder', icon: '☣️' },
    { id: 'raid', label: 'Sanal RAID (VRAID)', icon: '🧱' },
    { id: 'report', label: 'Adli Rapor (PDF)', icon: '📋' },
  ]

  return (
    <aside className="sidebar">
      <div className="sidebar-logo">
        <span className="logo-icon">🐺</span>
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
        <div className="version-info">Sürüm 1.0.0</div>
      </div>
    </aside>
  )
}

export default Sidebar
