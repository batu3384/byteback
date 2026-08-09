import React, { useState } from 'react'
import Sidebar from './components/Layout/Sidebar'
import Header from './components/Layout/Header'
import Dashboard from './components/Dashboard/Dashboard'
import ScanView from './components/ScanView/ScanView'

type Page = 'dashboard' | 'scan' | 'results' | 'hex' | 'imager' | 'smart'

function App(): React.ReactElement {
  const [activePage, setActivePage] = useState<Page>('dashboard')
  const [scanConfig, setScanConfig] = useState<{ driveIndex: number | null, scanType: string }>({ driveIndex: null, scanType: 'quick' })

  const handleStartScan = (driveIndex: number, scanType: string) => {
    setScanConfig({ driveIndex, scanType })
    setActivePage('scan')
  }

  const renderPage = () => {
    switch (activePage) {
      case 'dashboard': 
        return <Dashboard onStartScan={handleStartScan} />
      case 'scan':
        return <ScanView 
                 driveIndex={scanConfig.driveIndex} 
                 scanType={scanConfig.scanType} 
                 onCancel={() => setActivePage('dashboard')} 
               />
      default: return (
        <div className="placeholder-page">
          <h2>{activePage.toUpperCase()}</h2>
          <p>Coming in Phase 3/4</p>
        </div>
      )
    }
  }

  return (
    <div className="app-layout">
      <Sidebar activePage={activePage} onNavigate={setActivePage} />
      <div className="app-main">
        <Header title={activePage} />
        <main className="app-content">
          {renderPage()}
        </main>
      </div>
    </div>
  )
}

export default App
