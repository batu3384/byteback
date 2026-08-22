import React, { useState, useEffect, useRef, useCallback } from 'react'
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
import CaseView from './components/CaseView/CaseView'
import ScanRequiredPanel from './components/ScanRequiredPanel'
import InlineAlert from './components/InlineAlert'
import { hasValidScanId, isLiveScanStatus, isScanDependentPage, isDiskBusyPage } from '../shared/scan-required'
import { SCAN_STATUS, scanPhaseFromState } from '../shared/scan-session'
import type { ScanState } from '../shared/ipc-contract'

type Page = 'dashboard' | 'scan' | 'results' | 'hex' | 'imager' | 'smart' | 'shredder' | 'raid' | 'report' | 'search' | 'timeline' | 'case'

function App(): React.ReactElement {
  const [activePage, setActivePage] = useState<Page>('dashboard')
  const [scanConfig, setScanConfig] = useState<{ driveIndex: number | null, scanType: string }>({ driveIndex: null, scanType: 'quick' })
  const [selectedDrive, setSelectedDrive] = useState<number | null>(null)
  const [selectedDriveSectorSize, setSelectedDriveSectorSize] = useState<number>(512)
  
  // Global Scan State (Persists across tab changes)
  const [scanProgress, setScanProgress] = useState({ current: 0, total: 0, badSectors: [] as number[], phase: 'metadata' })
  const [scanStatus, setScanStatus] = useState('Bekleniyor...')
  const [scanElapsed, setScanElapsed] = useState(0)
  const [activeScanId, setActiveScanId] = useState<number>(-1)
  const [scanRowState, setScanRowState] = useState<ScanState | null>(null)
  const [dbError, setDbError] = useState<string | null>(null)
  const [sessionNote, setSessionNote] = useState<{ summary: string; path: string; lines: string[] } | null>(null)
  
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const activeScanIdRef = useRef(activeScanId)
  activeScanIdRef.current = activeScanId
  const scanBusy = isLiveScanStatus(scanStatus)

  const hydrateFromScanState = useCallback((state: ScanState) => {
    setActiveScanId(state.id)
    setScanRowState(state)
    setSelectedDrive(state.driveIndex)
    setScanConfig({ driveIndex: state.driveIndex, scanType: state.scanType })
    setScanProgress({
      current: state.scannedSectors,
      total: state.totalSectors > 0 ? state.totalSectors : 1,
      badSectors: [],
      phase: scanPhaseFromState(state),
    })
  }, [])

  useEffect(() => {
    window.api?.getDbStatus?.()
      .then((s) => {
        if (!s.ready) setDbError(s.error ?? 'Veritabanı başlatılamadı')
      })
      .catch(() => setDbError('Veritabanı durumu okunamadı'))
    window.api?.getSessionLog?.(60)
      .then((log) => {
        if (log?.summary) setSessionNote({ summary: log.summary, path: log.path, lines: log.lines ?? [] })
      })
      .catch(() => { /* günlük yoksa sessiz */ })

    if (!window.api?.getLatestUsableScanId || !window.api.getScanState) return
    window.api.getLatestUsableScanId()
      .then(async (id) => {
        if (id <= 0) return
        const state = await window.api!.getScanState(id)
        if (!state || state.id <= 0) return
        hydrateFromScanState(state)
        if (state.status === SCAN_STATUS.paused) {
          setScanStatus('Tarama Duraklatıldı — devam edilebilir')
        } else if (state.status === SCAN_STATUS.complete) {
          setScanStatus('Tarama Tamamlandı')
        }
      })
      .catch(() => { /* ilk açılışta kayıt yok */ })
  }, [hydrateFromScanState])

  // System Engine Ready
  useEffect(() => {
    if (window.api && window.api.getVersion) {
      window.api.getVersion().then((ver: string) => {
        console.log("Byteback Engine Ready. Version:", ver);
      }).catch((e: Error) => console.error("Engine failure:", e));
    }
    
    // Setup Global IPC Listeners ONLY ONCE
    let cleanupProgress: (() => void) | undefined
    let cleanupComplete: (() => void) | undefined

    if (window.api && window.api.onScanProgress) {
      cleanupProgress = window.api.onScanProgress((data: { current: number, total: number, badSectors?: number[], phase?: string }) => {
        setScanProgress({
          current: data.current,
          total: data.total,
          badSectors: data.badSectors ?? [],
          phase: data.phase ?? 'metadata',
        })
      })
    }

    if (window.api && window.api.onScanComplete) {
      cleanupComplete = window.api.onScanComplete(({ scanId, status }) => {
        if (scanId > 0 && activeScanIdRef.current > 0 && scanId !== activeScanIdRef.current) return
        if (scanId > 0) setActiveScanId(scanId)
        if (status === 1) setScanStatus('Tarama Tamamlandı')
        else if (status === 2) setScanStatus('Tarama İptal Edildi')
        else if (status === 4) setScanStatus('Tarama Duraklatıldı — devam edilebilir')
        else setScanStatus('Tarama Başarısız')
        if (timerRef.current) clearInterval(timerRef.current)
        // Only completed scans fill the bar; cancel/fail/pause keep honest position.
        if (status === 1) {
          setScanProgress(prev => ({ ...prev, current: prev.total > 0 ? prev.total : prev.current }))
        }
      })
    }

    return () => {
      if (cleanupProgress) cleanupProgress()
      if (cleanupComplete) cleanupComplete()
    }
  }, [])

  const failScan = (message: string) => {
    setScanStatus(message)
    if (timerRef.current) {
      clearInterval(timerRef.current)
      timerRef.current = null
    }
  }

  const startScanTimer = () => {
    if (timerRef.current) clearInterval(timerRef.current)
    timerRef.current = setInterval(() => {
      setScanElapsed(prev => prev + 1)
    }, 1000)
  }

  const handleStartScan = (driveIndex: number, scanType: string, scanOptions?: import('../shared/ipc-contract').ScanOptions) => {
    if (dbError) {
      failScan(`Veritabanı kullanılamıyor: ${dbError}`)
      return
    }
    const isResume = !!(scanOptions?.resumeScanId && scanOptions.resumeScanId > 0)
    setSelectedDrive(driveIndex)
    setScanConfig({ driveIndex, scanType })

    if (!isResume) {
      setScanProgress({ current: 0, total: 0, badSectors: [], phase: 'metadata' })
      setScanElapsed(0)
    } else if (window.api?.getScanState && scanOptions?.resumeScanId) {
      window.api.getScanState(scanOptions.resumeScanId)
        .then((state) => { if (state?.id > 0) hydrateFromScanState(state) })
        .catch(() => { /* resume yine de dener */ })
    }
    setScanStatus(isResume ? 'Tarama Devam Ediyor...' : 'Tarama Sürüyor...')
    setActivePage('scan')

    if (!window.api?.startScan) {
      failScan('Tarama API\'si kullanılamıyor. Native backend yüklü mü kontrol edin.')
      return
    }

    window.api.startScan(driveIndex, scanType, scanOptions)
      .then((id) => {
        if (id > 0) {
          setActiveScanId(id)
          startScanTimer()
        } else {
          failScan('Tarama başlatılamadı. Yönetici izni ve sürücü seçimini kontrol edin.')
        }
      })
      .catch((e: Error) => failScan(`Tarama hatası: ${e.message}`))
  }

  const handleStartRaidScan = (scanType: string) => {
    if (dbError) {
      failScan(`Veritabanı kullanılamıyor: ${dbError}`)
      return
    }
    setSelectedDrive(-1)
    setScanConfig({ driveIndex: -1, scanType })
    setScanProgress({ current: 0, total: 0, badSectors: [], phase: 'metadata' })
    setScanStatus('RAID Taraması Sürüyor...')
    setScanElapsed(0)
    setActivePage('scan')
    if (!window.api?.startScan) {
      failScan('RAID tarama API\'si kullanılamıyor.')
      return
    }
    window.api.startScan(-1, scanType)
      .then((id) => {
        if (id > 0) {
          setActiveScanId(id)
          startScanTimer()
        } else {
          failScan('RAID taraması başlatılamadı. Dizi kurulumunu ve Yönetici iznini kontrol edin.')
        }
      })
      .catch((e: Error) => failScan(`RAID tarama hatası: ${e.message}`))
  }

  const handleOpenPausedResults = (state: ScanState) => {
    hydrateFromScanState(state)
    setScanStatus('Tarama Duraklatıldı — devam edilebilir')
    setActivePage('results')
  }

  const handleClearScanData = async (): Promise<boolean> => {
    if (!window.api?.resetScanDatabase) return false
    const ok = await window.api.resetScanDatabase()
    if (ok) {
      setActiveScanId(-1)
      setScanRowState(null)
      setScanProgress({ current: 0, total: 0, badSectors: [], phase: 'metadata' })
      setScanStatus('Bekleniyor...')
      setScanElapsed(0)
      setScanConfig({ driveIndex: null, scanType: 'quick' })
      setActivePage('dashboard')
    }
    return ok
  }

  const handleStopScan = () => {
    if (window.api && window.api.stopScan) {
      window.api.stopScan()
    }
    setScanStatus('Durduruluyor...')
  }

  const handleAction = (page: Page, data?: any) => {
    if (data && data.driveIndex !== undefined) {
      setSelectedDrive(data.driveIndex)
      if (data.sectorSize) setSelectedDriveSectorSize(data.sectorSize)
    }
    if (isScanDependentPage(page) && !hasValidScanId(activeScanId)) return
    if (isDiskBusyPage(page) && scanBusy) return
    setActivePage(page)
  }

  const handleNavigate = (page: string) => {
    if (isScanDependentPage(page) && !hasValidScanId(activeScanId)) return
    if (isDiskBusyPage(page) && scanBusy) return
    setActivePage(page as Page)
  }

  const renderPage = () => {
    if (isScanDependentPage(activePage) && !hasValidScanId(activeScanId)) {
      return <ScanRequiredPanel onGoDashboard={() => setActivePage('dashboard')} />
    }

    switch (activePage) {
      case 'dashboard': 
        return (
          <Dashboard
            onStartScan={handleStartScan}
            onAction={handleAction}
            onOpenPausedResults={handleOpenPausedResults}
            onClearScanData={handleClearScanData}
            scanBusy={scanBusy}
          />
        )
      case 'scan':
        return <ScanView 
                 driveIndex={scanConfig.driveIndex} 
                 scanType={scanConfig.scanType} 
                 progress={scanProgress}
                 status={scanStatus}
                 elapsed={scanElapsed}
                 activeScanId={activeScanId}
                 onStop={handleStopScan}
                 onCancel={() => setActivePage('dashboard')} 
                 onViewResults={() => setActivePage('results')}
               />
      case 'results':
        return <ResultsView filesFound={[]} driveIndex={scanConfig.driveIndex} scanId={activeScanId} />
      case 'search':
        return <KeywordSearch scanId={activeScanId} />
      case 'timeline':
        return <TimelineView scanId={activeScanId} />
      case 'report':
        return <ReportGenerator scanId={activeScanId} scanElapsed={scanElapsed} scanState={scanRowState} />
      case 'hex':
        return <HexEditor driveIndex={selectedDrive} sectorSize={selectedDriveSectorSize} scanBusy={scanBusy} />
      case 'smart':
        return <SmartView driveIndex={selectedDrive} />
      case 'imager':
        return <ImagerView />
      case 'shredder':
        return <ShredderView />
      case 'raid':
        return <RaidBuilder onStartRaidScan={handleStartRaidScan} />
      case 'case':
        return <CaseView />
      default: 
        return (
          <Dashboard
            onStartScan={handleStartScan}
            onAction={handleAction}
            onOpenPausedResults={handleOpenPausedResults}
            onClearScanData={handleClearScanData}
            scanBusy={scanBusy}
          />
        )
    }
  }

  return (
    <div className="app-layout">
      <Sidebar activePage={activePage} activeScanId={activeScanId} scanState={scanRowState} scanBusy={scanBusy} onNavigate={handleNavigate} />
      <div className="app-main">
        <Header
          title={activePage}
          scanBusy={scanBusy}
          scanPercent={scanProgress.total > 0 ? Math.min(100, Math.floor((scanProgress.current / scanProgress.total) * 100)) : undefined}
          onOpenScan={() => setActivePage('scan')}
        />
        <main className="app-content">
          {dbError && (
            <InlineAlert variant="error" title="Veritabanı kullanılamıyor">
              Tarama, kurtarma ve rapor SQLite&apos;a bağlıdır. Hata: {dbError}. Uygulamayı yeniden başlatın.
            </InlineAlert>
          )}
          {sessionNote && !sessionNote.summary.includes('tarama kaydı yok') && !sessionNote.summary.includes('tamamlandı') && (
            <InlineAlert
              variant={sessionNote.summary.includes('çöktü') || sessionNote.summary.includes('hata') ? 'error' : 'warning'}
              title="Son tarama"
              onDismiss={() => setSessionNote(null)}
            >
              <div>{sessionNote.summary}</div>
              {sessionNote.path ? (
                <div style={{ marginTop: '6px', fontSize: '0.8rem', color: 'var(--text-muted)', wordBreak: 'break-all' }}>
                  Günlük: {sessionNote.path}
                </div>
              ) : null}
              {sessionNote.lines.length > 0 ? (
                <pre style={{ marginTop: '8px', maxHeight: '140px', overflow: 'auto', fontSize: '0.72rem', whiteSpace: 'pre-wrap' }}>
                  {sessionNote.lines.slice(-12).join('\n')}
                </pre>
              ) : null}
            </InlineAlert>
          )}
          {renderPage()}
        </main>
      </div>
    </div>
  )
}

export default App
