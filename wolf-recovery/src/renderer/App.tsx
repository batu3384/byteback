import React, { useState } from 'react'
import Sidebar from './components/Layout/Sidebar'
import Header from './components/Layout/Header'
import Dashboard from './components/Dashboard/Dashboard'
import ScanView from './components/ScanView/ScanView'
import ResultsView from './components/ResultsView/ResultsView'
import HexEditor from './components/HexEditor/HexEditor'
import SmartView from './components/SmartView/SmartView'
import ImagerView from './components/ImagerView/ImagerView'

type Page = 'dashboard' | 'scan' | 'results' | 'hex' | 'imager' | 'smart'

function App(): React.ReactElement {
  const [activePage, setActivePage] = useState<Page>('dashboard')
  const [scanConfig, setScanConfig] = useState<{ driveIndex: number | null, scanType: string }>({ driveIndex: null, scanType: 'quick' })
  const [selectedDrive, setSelectedDrive] = useState<number | null>(null)
  const [selectedDriveSectorSize, setSelectedDriveSectorSize] = useState<number>(512)
  
  // Lifted Scan State
  const [filesFound, setFilesFound] = useState<any[]>([])
  const [scanProgress, setScanProgress] = useState({ current: 0, total: 100 })
  const [scanStatus, setScanStatus] = useState('Bekliyor')
  const [scanElapsed, setScanElapsed] = useState(0)

  const handleStartScan = (driveIndex: number, scanType: string) => {
    setSelectedDrive(driveIndex)
    setScanConfig({ driveIndex, scanType })
    // Reset state for new scan
    setFilesFound([])
    setScanProgress({ current: 0, total: 100 })
    setScanStatus('Tarama Sürüyor...')
    setScanElapsed(0)
    setActivePage('scan')
  }

  const handleAction = (page: Page, data?: any) => {
    if (data && data.driveIndex !== undefined) {
      setSelectedDrive(data.driveIndex)
      if (data.sectorSize) setSelectedDriveSectorSize(data.sectorSize)
    }
    setActivePage(page)
  }

  const renderPage = () => {
    switch (activePage) {
      case 'dashboard': 
        return <Dashboard onStartScan={handleStartScan} onAction={handleAction} />
      case 'scan':
        return <ScanView 
                 driveIndex={scanConfig.driveIndex} 
                 scanType={scanConfig.scanType} 
                 filesFound={filesFound}
                 setFilesFound={setFilesFound}
                 progress={scanProgress}
                 setProgress={setScanProgress}
                 status={scanStatus}
                 setStatus={setScanStatus}
                 elapsed={scanElapsed}
                 setElapsed={setScanElapsed}
                 onCancel={() => setActivePage('dashboard')} 
                 onViewResults={() => setActivePage('results')}
               />
      case 'results':
        return <ResultsView filesFound={filesFound} />
      case 'hex':
        return <HexEditor driveIndex={selectedDrive} sectorSize={selectedDriveSectorSize} />
      case 'smart':
        return <SmartView driveIndex={selectedDrive} />
      case 'imager':
        return <ImagerView />
      default: 
        return <Dashboard onStartScan={handleStartScan} onAction={handleAction} />
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
