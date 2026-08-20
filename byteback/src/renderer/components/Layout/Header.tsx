import React, { useEffect, useState } from 'react'
import './Header.css'
import { Sun, Moon } from 'lucide-react'

interface HeaderProps {
  title: string
}

const pageTitles: Record<string, string> = {
  dashboard: 'Ana Ekran',
  scan: 'Aktif Tarama',
  results: 'Kurtarma Sonuçları',
  hex: 'Hex İnceleyici',
  imager: 'İmaj (RAW / E01)',
  smart: 'S.M.A.R.T. Durumu',
  search: 'Kelime Arama',
  report: 'Adli Rapor',
  shredder: 'Veri Yok Edici',
  raid: 'Sanal RAID Oluştur',
  timeline: 'Olay Zaman Çizelgesi',
  case: 'Dava / NSRL',
}

function Header({ title }: HeaderProps): React.ReactElement {
  const [theme, setTheme] = useState<'dark' | 'light'>(() =>
    (localStorage.getItem('byteback-theme') as 'dark' | 'light') || 'dark'
  )

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme)
    localStorage.setItem('byteback-theme', theme)
  }, [theme])

  return (
    <header className="app-header">
      <div className="header-title">
        <h2>{pageTitles[title] || title.toUpperCase()}</h2>
      </div>
      <div className="header-actions">
        <button
          className="icon-btn"
          type="button"
          aria-label={theme === 'dark' ? 'Açık temaya geç' : 'Koyu temaya geç'}
          title={theme === 'dark' ? 'Açık temaya geç' : 'Koyu temaya geç'}
          onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')}
        >
          {theme === 'dark' ? <Sun size={18} /> : <Moon size={18} />}
        </button>
      </div>
    </header>
  )
}

export default Header
