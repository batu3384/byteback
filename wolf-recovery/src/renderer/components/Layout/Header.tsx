import React from 'react'
import './Header.css'

interface HeaderProps {
  title: string
}

const pageTitles: Record<string, string> = {
  dashboard: 'Ana Ekran',
  scan: 'Aktif Tarama',
  results: 'Kurtarma Sonuçları',
  hex: 'Hex İnceleyici',
  imager: 'Disk İmaj Alma',
  smart: 'SMART Sağlık Durumu'
}

function Header({ title }: HeaderProps): React.ReactElement {
  return (
    <header className="app-header">
      <div className="header-title">
        <h2>{pageTitles[title] || title.toUpperCase()}</h2>
      </div>
      <div className="header-actions">
        <button className="icon-btn" title="Ayarlar">⚙️</button>
        <button className="icon-btn" title="Yardım">❓</button>
      </div>
    </header>
  )
}

export default Header
