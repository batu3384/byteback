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
import { hasValidScanId, isLiveScanPhase, isScanDependentPage, isDiskBusyPage, scanPhaseFromStatusCode, type ScanPhase } from '../shared/scan-required'
import { SCAN_STATUS, scanPhaseFromState } from '../shared/scan-session'
import type { ScanState } from '../shared/ipc-contract'
import { t, tFormat, useI18n } from './i18n'

type Page = 'dashboard' | 'scan' | 'results' | 'hex' | 'imager' | 'smart' | 'shredder' | 'raid' | 'report' | 'search' | 'timeline' | 'case'

/** Status line text as an i18n key (+ optional engine error detail) so the
 *  active-scan screen translates instead of carrying hardcoded TR strings. */
export interface ScanStatusMessage { key: string; err?: string }

function App(): React.ReactElement {
  const { t } = useI18n() // re-renders on language switch so keyed status text updates
  const [activePage, setActivePage] = useState<Page>('dashboard')
  const [scanConfig, setScanConfig] = useState<{ driveIndex: number | null, scanType: string }>({ driveIndex: null, scanType: 'quick' })
  const [selectedDrive, setSelectedDrive] = useState<number | null>(null)
  const [selectedDriveSectorSize, setSelectedDriveSectorSize] = useState<number>(512)

  // Global Scan State (Persists across tab changes)
  const [scanProgress, setScanProgress] = useState({ current: 0, total: 0, badSectors: [] as number[], phase: 'metadata' })
  const [scanStatus, setScanStatus] = useState<ScanStatusMessage>({ key: 'scan.waiting' })
  const [scanPhase, setScanPhase] = useState<ScanPhase>('idle')
  const [scanElapsed, setScanElapsed] = useState(0)
  const [activeScanId, setActiveScanId] = useState<number>(-1)
  const [scanRowState, setScanRowState] = useState<ScanState | null>(null)
  const [dbError, setDbError] = useState<string | null>(null)
  const [sessionNote, setSessionNote] = useState<{ summary: string; path: string; lines: string[]; code: string } | null>(null)
  // CA-032: imaging runs in the main process; the flag survives navigation so
  // the progress card re-appears when the user returns to the imager page.
  const [imagingActive, setImagingActive] = useState(false)

  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const activeScanIdRef = useRef(activeScanId)
  activeScanIdRef.current = activeScanId
  // W8: set synchronously the moment a scan is requested — the async startup
  // hydration must never overwrite a scan the user just started.
  const scanStartAttemptRef = useRef(0)
  // PERF: coalesced progress flush state (see onScanProgress below).
  const pendingProgressRef = useRef<{ current: number, total: number, badSectors: number[], phase: string, phaseCurrent?: number, phaseTotal?: number } | null>(null)
  const progressFlushTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const lastProgressFlushRef = useRef(0)
  const scanBusy = isLiveScanPhase(scanPhase)

  const hydrateFromScanState = useCallback((state: ScanState) => {
    setActiveScanId(state.id)
    setScanRowState(state)
    setSelectedDrive(state.driveIndex)
    setScanConfig({ driveIndex: state.driveIndex, scanType: state.scanType })
    // Restart/resume: the elapsed clock must not reset to 00:00:00 for a
    // session that already ran — both columns are unix seconds in SQLite.
    if (state.startedAt && state.updatedAt && state.updatedAt > state.startedAt) {
      setScanElapsed(state.updatedAt - state.startedAt)
    }
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
        if (!s.ready) setDbError(s.error ?? t('dash.dbInitError'))
      })
      .catch(() => setDbError(t('dash.dbStatusError')))
    window.api?.getSessionLog?.(60)
      .then((log) => {
        if (log?.summary) setSessionNote({ summary: log.summary, path: log.path, lines: log.lines ?? [], code: log.code ?? 'incomplete' })
      })
      .catch(() => { /* günlük yoksa sessiz */ })

    if (!window.api?.getLatestUsableScanId || !window.api.getScanState) return
    window.api.getLatestUsableScanId()
      .then(async (id) => {
        if (id <= 0) return
        // W8: a scan started while hydration was in flight owns the UI —
        // stale hydration would hijack activeScanId and drop every event
        // of the new scan.
        if (scanStartAttemptRef.current > 0) return
        const state = await window.api!.getScanState(id)
        if (!state || state.id <= 0) return
        if (scanStartAttemptRef.current > 0) return
        hydrateFromScanState(state)
        if (state.status === SCAN_STATUS.paused) {
          setScanStatus({ key: 'scan.pausedResumable' })
          setScanPhase('paused')
        } else if (state.status === SCAN_STATUS.complete) {
          setScanStatus({ key: 'scan.finished' })
          setScanPhase('complete')
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
    let cleanupImaging: (() => void) | undefined

    if (window.api && window.api.onScanProgress) {
      // PERF: native throttles to 250 ms, but every event was a fresh state
      // object → whole-tree re-render 4×/s (jank on heavy pages). Coalesce to
      // at most one render per 600 ms; trailing flush keeps the bar live.
      const flushProgress = () => {
        const p = pendingProgressRef.current
        if (!p) return
        pendingProgressRef.current = null
        setScanProgress(p)
        setScanPhase((prev) => (prev === 'starting' ? 'running' : prev))
      }
      cleanupProgress = window.api.onScanProgress((data: { scanId?: number, current: number, total: number, badSectors?: number[], phase?: string, phaseCurrent?: number, phaseTotal?: number }) => {
        // CA-016: progress events carry the scan id; a stale scan can no
        // longer overwrite the active scan's progress bar.
        if (data.scanId && data.scanId > 0 && activeScanIdRef.current > 0 && data.scanId !== activeScanIdRef.current) return
        pendingProgressRef.current = {
          current: data.current,
          total: data.total,
          badSectors: data.badSectors ?? [],
          phase: data.phase ?? 'metadata',
          phaseCurrent: data.phaseCurrent,
          phaseTotal: data.phaseTotal,
        }
        const now = Date.now()
        if (now - lastProgressFlushRef.current >= 600) {
          lastProgressFlushRef.current = now
          flushProgress()
        } else if (progressFlushTimerRef.current == null) {
          progressFlushTimerRef.current = setTimeout(() => {
            progressFlushTimerRef.current = null
            lastProgressFlushRef.current = Date.now()
            flushProgress()
          }, 500)
        }
      })
    }

    if (window.api && window.api.onScanComplete) {
      cleanupComplete = window.api.onScanComplete(({ scanId, status }) => {
        if (scanId > 0 && activeScanIdRef.current > 0 && scanId !== activeScanIdRef.current) return
        if (scanId > 0) setActiveScanId(scanId)
        setScanPhase(scanPhaseFromStatusCode(status))
        if (status === 1) setScanStatus({ key: 'scan.finished' })
        else if (status === 2) setScanStatus({ key: 'scan.cancelled' })
        else if (status === 4) setScanStatus({ key: 'scan.pausedResumable' })
        else setScanStatus({ key: 'scan.failed' })
        if (timerRef.current) clearInterval(timerRef.current)
        // A pending throttled progress flush would repaint the bar with stale
        // values after completion — drop it.
        pendingProgressRef.current = null
        if (progressFlushTimerRef.current) { clearTimeout(progressFlushTimerRef.current); progressFlushTimerRef.current = null }
        // CA-014: refresh the scan row so the report nav unlocks without an
        // app restart.
        if (scanId > 0 && window.api?.getScanState) {
          window.api.getScanState(scanId)
            .then((state) => { if (state?.id > 0) setScanRowState(state) })
            .catch(() => { /* keep previous row state */ })
        }
        // Only completed scans fill the bar; cancel/fail/pause keep honest position.
        if (status === 1) {
          setScanProgress(prev => ({ ...prev, current: prev.total > 0 ? prev.total : prev.current }))
        }
      })
    }

    // W6: imaging completion/error must reset the App-level flag even while
    // the imager page is unmounted — its own listener dies with the page and
    // the flag would stick true until restart.
    if (window.api && window.api.onImagingProgress) {
      cleanupImaging = window.api.onImagingProgress((data: { current: number; total: number }) => {
        if (data.total === 0 || data.current >= data.total) setImagingActive(false)
      })
    }

    return () => {
      if (cleanupProgress) cleanupProgress()
      if (cleanupComplete) cleanupComplete()
      if (cleanupImaging) cleanupImaging()
    }
  }, [])

  const failScan = (key: string, err?: string) => {
    setScanStatus({ key, err })
    setScanPhase('failed')
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
      failScan('scan.failedDb', dbError)
      return
    }
    scanStartAttemptRef.current++
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
    setScanStatus({ key: isResume ? 'scan.resuming' : 'scan.running' })
    setScanPhase('starting')
    setActivePage('scan')

    if (!window.api?.startScan) {
      // Release the hydration gate — a failed attempt must not block startup
      // hydration of the last usable scan for the rest of the session.
      scanStartAttemptRef.current--
      failScan('scan.apiMissing')
      return
    }

    window.api.startScan(driveIndex, scanType, scanOptions)
      .then((id) => {
        if (id > 0) {
          setActiveScanId(id)
          startScanTimer()
        } else {
          scanStartAttemptRef.current--
          failScan('scan.startFailed')
        }
      })
      .catch((e: Error) => {
        scanStartAttemptRef.current--
        failScan('scan.failedWith', e.message)
      })
  }

  const handleStartRaidScan = (scanType: string) => {
    if (dbError) {
      failScan('scan.failedDb', dbError)
      return
    }
    scanStartAttemptRef.current++
    setSelectedDrive(-1)
    setScanConfig({ driveIndex: -1, scanType })
    setScanProgress({ current: 0, total: 0, badSectors: [], phase: 'metadata' })
    setScanStatus({ key: 'scan.raidRunning' })
    setScanPhase('starting')
    setScanElapsed(0)
    setActivePage('scan')
    if (!window.api?.startScan) {
      scanStartAttemptRef.current--
      failScan('scan.raidApiMissing')
      return
    }
    window.api.startScan(-1, scanType)
      .then((id) => {
        if (id > 0) {
          setActiveScanId(id)
          startScanTimer()
        } else {
          scanStartAttemptRef.current--
          failScan('scan.raidStartFailed')
        }
      })
      .catch((e: Error) => {
        scanStartAttemptRef.current--
        failScan('scan.raidFailedWith', e.message)
      })
  }

  const handleOpenPausedResults = (state: ScanState) => {
    hydrateFromScanState(state)
    setScanStatus({ key: 'scan.pausedResumable' })
    setScanPhase('paused')
    setActivePage('results')
  }

  const handleClearScanData = async (): Promise<boolean> => {
    if (!window.api?.resetScanDatabase) return false
    const ok = await window.api.resetScanDatabase()
    if (ok) {
      setActiveScanId(-1)
      setScanRowState(null)
      setScanProgress({ current: 0, total: 0, badSectors: [], phase: 'metadata' })
      setScanStatus({ key: 'scan.waiting' })
      setScanPhase('idle')
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
    setScanStatus({ key: 'scan.stoppingStatus' })
    setScanPhase('stopping')
  }

  const handleAction = (page: Page, data?: any) => {
    if (data && data.driveIndex !== undefined) {
      setSelectedDrive(data.driveIndex)
      if (data.sectorSize) setSelectedDriveSectorSize(data.sectorSize)
    }
    if (isScanDependentPage(page) && !hasValidScanId(activeScanId)) return
    if (page === 'scan' && scanPhase === 'idle' && !hasValidScanId(activeScanId)) return
    if (isDiskBusyPage(page) && scanBusy) return
    setActivePage(page)
  }

  const handleNavigate = (page: string) => {
    if (isScanDependentPage(page) && !hasValidScanId(activeScanId)) return
    // CA-013: no live or hydrated scan session — the active-scan page would
    // render a bogus "Sürücü undefined taranıyor" with a live stop button.
    if (page === 'scan' && scanPhase === 'idle' && !hasValidScanId(activeScanId)) return
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
        if (scanPhase === 'idle' && !hasValidScanId(activeScanId)) {
          return <ScanRequiredPanel onGoDashboard={() => setActivePage('dashboard')} />
        }
        return <ScanView
                 driveIndex={scanConfig.driveIndex}
                 scanType={scanConfig.scanType}
                 progress={scanProgress}
                 status={scanStatus}
                 phase={scanPhase}
                 elapsed={scanElapsed}
                 activeScanId={activeScanId}
                 onStop={handleStopScan}
                 onCancel={() => setActivePage('dashboard')}
                 onViewResults={() => setActivePage('results')}
               />
      case 'results':
        return <ResultsView filesFound={[]} driveIndex={scanConfig.driveIndex} scanId={activeScanId} scanBusy={scanBusy} />
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
        return <ImagerView imagingActive={imagingActive} onImagingStateChange={setImagingActive} />
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
          onOpenScan={() => handleNavigate('scan')}
        />
        <main className="app-content">
          {dbError && (
            <InlineAlert variant="error" title={t('dash.dbAlertTitle')}>
              {tFormat('dash.dbAlertBody', { err: dbError })}
            </InlineAlert>
          )}
          {sessionNote && sessionNote.code !== 'no_scan' && sessionNote.code !== 'complete' && (
            <InlineAlert
              variant={sessionNote.code === 'crash' || sessionNote.code === 'crash_during_scan' || sessionNote.code === 'fail' ? 'error' : 'warning'}
              title={t('dash.lastScanTitle')}
              onDismiss={() => setSessionNote(null)}
            >
              <div>{sessionNote.summary}</div>
              {sessionNote.path ? (
                <div style={{ marginTop: '6px', fontSize: '0.8rem', color: 'var(--text-muted)', wordBreak: 'break-all' }}>
                  {tFormat('dash.logPrefix', { path: sessionNote.path })}
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
