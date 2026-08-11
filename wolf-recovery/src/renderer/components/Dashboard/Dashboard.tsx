import React, { useEffect, useState } from 'react'
import DriveCard from './DriveCard'
import type { DriveInfo } from '../../../shared/types'
import './Dashboard.css'

interface DashboardProps {
  onStartScan?: (driveIndex: number, scanType: string) => void
  onAction?: (page: any, data?: any) => void
}

function Dashboard({ onStartScan, onAction }: DashboardProps): React.ReactElement {
  const [drives, setDrives] = useState<DriveInfo[]>([])
  const [loading, setLoading] = useState(true)
  const [isAdmin, setIsAdmin] = useState<boolean | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [activeSession, setActiveSession] = useState<any>(null)

  const fetchDrives = async () => {
    setLoading(true)
    setError(null)
    try {
      if (window.api) {
        const [adminStatus, driveList] = await Promise.all([
          window.api.isAdmin(),
          window.api.listDrives()
        ])
        setIsAdmin(adminStatus)
        setDrives(driveList as DriveInfo[])
        if (!driveList || driveList.length === 0) {
          setError('Hiçbir fiziksel sürücü tespit edilemedi. Uygulama yönetici izni olmadan çalışıyorsa bazı sürücüler gizlenebilir.')
        }
      } else {
        setError('Kritik Hata: window.api bulunamadı! IPC Köprüsü yüklenemedi.')
      }
    } catch (err: any) {
      console.error('Sürücüler alınırken hata:', err)
      setError('Sürücü listesi alınırken hata oluştu: ' + (err?.message || String(err)))
      setDrives([])
    } finally {
      setLoading(false)
    }
  }

  const checkActiveSession = async () => {
    if (window.api && window.api.getScanState) {
      try {
        const state = await window.api.getScanState(1) // hardcoded scanId=1 for MVP
        if (state && (state.status === 0 || state.status === 1)) {
          setActiveSession(state)
        }
      } catch (err) {
        console.error("No active session found")
      }
    }
  }

  useEffect(() => {
    fetchDrives()
    checkActiveSession()
  }, [])

  return (
    <div className="dashboard">
      {isAdmin === false && (
        <div className="admin-banner glass-panel">
          <span className="admin-banner-icon">🛡️</span>
          <div className="admin-banner-text">
            <strong>Yönetici İzni Yok</strong> — Sürücüler listelenebilir ancak tarama ve sektör okuma gibi işlemler için uygulamayı <em>Yönetici Olarak Çalıştır</em> ile yeniden başlatmanız gerekir.
          </div>
        </div>
      )}

      {activeSession && (
        <div className="resume-banner glass-panel" style={{ borderLeft: '4px solid #00f3ff', marginBottom: '20px', display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <div>
            <h3>Yarım Kalan Tarama Bulundu</h3>
            <p>Sürücü {activeSession.driveIndex} üzerinde {activeSession.scanType} taraması yapılıyordu. ({activeSession.scannedSectors} / {activeSession.totalSectors} sektör)</p>
          </div>
          <button className="btn-primary" onClick={() => onAction && onAction('scan', { driveIndex: activeSession.driveIndex, scanType: activeSession.scanType })}>
            Devam Et
          </button>
        </div>
      )}

      <div className="dashboard-header">
        <div>
          <h2>Bağlı Sürücüler</h2>
          <p className="subtitle">Sisteminizde tespit edilen fiziksel diskler</p>
        </div>
        <button className="btn-secondary" onClick={fetchDrives}>
          <span className="icon">↻</span> Yenile
        </button>
      </div>

      {loading ? (
        <div className="loading-state">
          <div className="spinner"></div>
          <p>Sürücüler taranıyor...</p>
        </div>
      ) : drives.length === 0 ? (
        <div className="empty-state glass-panel">
          <h3>Sürücü Bulunamadı</h3>
          <p>{error || 'Sisteminizde desteklenen fiziksel bir disk tespit edilemedi.'}</p>
          <button className="btn-secondary" onClick={fetchDrives} style={{ marginTop: '12px' }}>
            Tekrar Dene
          </button>
        </div>
      ) : (
        <div className="drive-grid">
          {drives.map((drive) => (
            <DriveCard 
              key={drive.index} 
              drive={drive} 
              onStartScan={onStartScan} 
              onAction={onAction}
              isAdmin={isAdmin ?? false}
            />
          ))}
        </div>
      )}

      <div className="dashboard-stats glass-panel">
        <div className="stat-card">
          <span className="stat-value">{drives.length}</span>
          <span className="stat-label">Tespit Edilen Sürücü</span>
        </div>
        <div className="stat-card">
          <span className="stat-value">0</span>
          <span className="stat-label">Aktif Tarama</span>
        </div>
        <div className="stat-card">
          <span className="stat-value">0</span>
          <span className="stat-label">Kurtarılan Dosya</span>
        </div>
      </div>
    </div>
  )
}

export default Dashboard

