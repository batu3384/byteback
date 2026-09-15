import React, { useEffect, useState } from 'react'
import './Header.css'
import { Sun, Moon } from 'lucide-react'
import { useI18n, tFormat } from '../../i18n'

const pageTitleKeys: Record<string, string> = {
  dashboard: 'title.dashboard',
  scan: 'title.scan',
  results: 'title.results',
  hex: 'title.hex',
  imager: 'title.imager',
  smart: 'title.smart',
  search: 'title.search',
  report: 'title.report',
  shredder: 'title.shredder',
  raid: 'title.raid',
  timeline: 'title.timeline',
  case: 'title.case',
}

interface HeaderProps {
  title: string
  caseNumber?: string
  scanBusy?: boolean
  scanPercent?: number
  onOpenScan?: () => void
}

function Header({ title, caseNumber, scanBusy, scanPercent, onOpenScan }: HeaderProps): React.ReactElement {
  const { t, lang, setLang } = useI18n()
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
        <h2>{pageTitleKeys[title] ? t(pageTitleKeys[title]) : title.toUpperCase()}</h2>
        {caseNumber?.trim() ? (
          <span className="header-case">{tFormat('chrome.statusCase', { n: caseNumber.trim() })}</span>
        ) : null}
      </div>
      <div className="header-actions">
        {scanBusy && onOpenScan && title !== 'scan' && (
          <button type="button" className="scan-pill" onClick={onOpenScan}>
            {t('header.scanRunning')}{typeof scanPercent === 'number' ? ` · ${tFormat('common.percent', { n: String(scanPercent) })}` : ''}
          </button>
        )}
        <button
          className="icon-btn"
          type="button"
          aria-label={t('header.toEnglish')}
          title={t('header.toEnglish')}
          onClick={() => setLang(lang === 'tr' ? 'en' : 'tr')}
        >
          <span className="lang-code">{lang === 'tr' ? 'EN' : 'TR'}</span>
        </button>
        <button
          className="icon-btn"
          type="button"
          aria-label={theme === 'dark' ? t('header.toLightTheme') : t('header.toDarkTheme')}
          title={theme === 'dark' ? t('header.toLightTheme') : t('header.toDarkTheme')}
          onClick={() => setTheme(theme === 'dark' ? 'light' : 'dark')}
        >
          {theme === 'dark' ? <Sun size={18} /> : <Moon size={18} />}
        </button>
      </div>
    </header>
  )
}

export default Header
