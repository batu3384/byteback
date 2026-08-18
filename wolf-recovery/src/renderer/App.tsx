import React, { useState, useEffect, useRef } from 'react'
import Sidebar from './components/Layout/Sidebar'
import Header from './components/Layout/Header'
import Dashboard from './components/Dashboard/Dashboard'
import ScanView from './components/ScanView/ScanView'
import ResultsView from './components/ResultsView/ResultsView'
import HexEditor from './components/HexEditor/HexEditor'
import SmartView from './components/SmartView/SmartView'
import ImagerView from './components/ImagerView/ImagerView'
import ShredderView from './components/ShredderView/ShredderView'
import RaidBuilder from './components/VirtualRaid/RaidBuilder'
import ReportGenerator from './components/ReportView/ReportGenerator'
import KeywordSearch from './components/SearchView/KeywordSearch'
import TimelineView from './components/TimelineView/TimelineView'

type Page = 'dashboard' | 'scan' | 'results' | 'hex' | 'imager' | 'smart' | 'shredder' | 'raid' | 'report' | 'search' | 'timeline'

function App(): React.ReactElement {
  const [activePage, setActivePage] = useState<Page>('dashboard')
  const [scanConfig, setScanConfig] = useState<{ driveIndex: number | null, scanType: string }>({ driveIndex: null, scanType: 'quick' })
  const [selectedDrive, setSelectedDrive] = useState<number | null>(null)
  const [selectedDriveSectorSize, setSelectedDriveSectorSize] = useState<number>(512)
  
  // Global Scan State (Persists across tab changes)
  const [filesFound, setFilesFound] = useState<any[]>([])
  const [scanProgress, setScanProgress] = useState({ current: 0, total: 100, badSectors: [] as number[] })
  const [scanStatus, setScanStatus] = useState('Bekleniyor...')
  const [scanElapsed, setScanElapsed] = useState(0)
  const [activeScanId, setActiveScanId] = useState<number>(-1)
  
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null)

  // System Engine Ready
  useEffect(() => {
    if (window.api && window.api.getVersion) {
      window.api.getVersion().then((ver: string) => {
        console.log("Wolf Engine Ready. Version:", ver);
      }).catch((e: Error) => console.error("Engine failure:", e));
    }
    
    // Setup Global IPC Listeners ONLY ONCE
    let cleanupProgress: (() => void) | undefined
    let cleanupFileFound: (() => void) | undefined

    if (window.api && window.api.onScanProgress) {
      cleanupProgress = window.api.onScanProgress((data: { current: number, total: number, badSectors?: number[] }) => {
        setScanProgress({ current: data.current, total: data.total, badSectors: data.badSectors ?? [] })
        if (data.current >= data.total && data.total > 0) {
          setScanStatus('Tarama Tamamlandı')
          if (timerRef.current) clearInterval(timerRef.current)
        }
      })
    }

    if (window.api && window.api.onScanFileFound) {
      // Live file-found stream from the scan engine. We de-duplicate by file id
      // (the engine emits a record per discovered file) and cap the in-memory
      // buffer to avoid unbounded growth during huge scans; ScanView/ResultsView
      // continue to paginate authoritative data from the SQLite store, this list
      // is for instant UI feedback during scanning.
      cleanupFileFound = window.api.onScanFileFound((data) => {
        setFilesFound(prev => {
          if (prev.length >= 5000) return prev
          if (typeof data.id === 'number' && data.id >= 0) {
            const idx = prev.findIndex(f => f.id === data.id)
            if (idx >= 0) {
              const next = [...prev]
              next[idx] = { ...prev[idx], ...data }
              return next
            }
          }
          return [...prev, data]
        })
      })
    }

    return () => {
      if (cleanupProgress) cleanupProgress()
      if (cleanupFileFound) cleanupFileFound()
    }
  }, []);

  const handleStartScan = (driveIndex: number, scanType: string) => {
    setSelectedDrive(driveIndex)
    setScanConfig({ driveIndex, scanType })
    
    // Reset Global State
    setFilesFound([])
    setScanProgress({ current: 0, total: 100, badSectors: [] })
    setScanStatus('Tarama Sürüyor...')
    setScanElapsed(0)
    setActivePage('scan')
    
    // Start Engine Scan
    if (window.api && window.api.startScan) {
      window.api.startScan(driveIndex, scanType).then(id => {
        if (id > 0) setActiveScanId(id)
      })
    }

    // Start Timer
    if (timerRef.current) clearInterval(timerRef.current)
    timerRef.current = setInterval(() => {
      setScanElapsed(prev => prev + 1)
    }, 1000)
  }

  const handleStopScan = () => {
    if (window.api && window.api.stopScan) {
      window.api.stopScan()
    }
    if (timerRef.current) clearInterval(timerRef.current)
    setScanStatus('Tarama İptal Edildi')
    setActiveScanId(-1)
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
                 status={scanStatus}
                 elapsed={scanElapsed}
                 activeScanId={activeScanId}
                 onStop={handleStopScan}
                 onCancel={() => setActivePage('dashboard')} 
                 onViewResults={() => setActivePage('results')}
               />
      case 'results':
        return <ResultsView filesFound={filesFound} driveIndex={scanConfig.driveIndex} scanId={activeScanId} />
      case 'search':
        return <KeywordSearch filesFound={filesFound} />
      case 'timeline':
        return <TimelineView scanId={activeScanId} />
      case 'report':
        return <ReportGenerator scanResults={{ totalFiles: filesFound.length, recoverableFiles: filesFound.filter(f => f.status === 0).length, partialFiles: filesFound.filter(f => f.status !== 0).length, duration: scanElapsed + ' sn' }} filesFound={filesFound} />
      case 'hex':
        return <HexEditor driveIndex={selectedDrive} sectorSize={selectedDriveSectorSize} />
      case 'smart':
        return <SmartView driveIndex={selectedDrive} />
      case 'imager':
        return <ImagerView />
      case 'shredder':
        return <ShredderView drives={[]} /> // Will fetch drives via Dashboard or pass down
      case 'raid':
        return <RaidBuilder />
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
