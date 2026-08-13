import React from 'react'
import './Header.css'
import { Settings, HelpCircle, Bell } from 'lucide-react'

interface HeaderProps {
  title: string
}

const pageTitles: Record<string, string> = {
  dashboard: 'Ana Ekran',
  scan: 'Aktif Tarama',
  results: 'Kurtarma Sonuçları',
  hex: 'Hex İnceleyici',
  imager: 'İmaj Alma (DD)',
  smart: 'S.M.A.R.T. Durumu',
  search: 'Kelime Arama',
  report: 'Adli Rapor',
  shredder: 'Veri Yok Edici',
  raid: 'Sanal RAID Oluştur'
}

function Header({ title }: HeaderProps): React.ReactElement {
  return (
    <header className="app-header">
      <div className="header-title">
        <h2>{pageTitles[title] || title.toUpperCase()}</h2>
      </div>
      <div className="header-actions">
        <button className="icon-btn" title="Bildirimler"><Bell size={18} /></button>
        <button className="icon-btn" title="Ayarlar"><Settings size={18} /></button>
        <button className="icon-btn" title="Yardım ve Belgeler"><HelpCircle size={18} /></button>
      </div>
    </header>
  )
}

export default Header
