import React, { useEffect, useState } from 'react'
import DriveCard from './DriveCard'
import type { DriveInfo } from '../../../shared/types'
import './Dashboard.css'
import { ShieldAlert, RotateCw, HardDrive, RefreshCw, Activity, FolderCheck, Play } from 'lucide-react'

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
  const [latestScan, setLatestScan] = useState<any>(null)
  const [fvekHex, setFvekHex] = useState('')
  const [fvekStatus, setFvekStatus] = useState<string | null>(null)

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
        setError('Kritik Hata: window.api bulunamadı! IPC Köprüsü yüklenemedi. Lütfen uygulamayı masaüstü modunda çalıştırın.')
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
    // CA-008 fix: query the REAL latest scan instead of hardcoded id 1.
    if (window.api?.getLatestScanId && window.api.getScanState) {
      try {
        const latestId = await window.api.getLatestScanId()
        if (latestId > 0) {
          const state = await window.api.getScanState(latestId)
          if (state) {
            setLatestScan(state)
            if (state.status === 0) setActiveSession(state)
            else setActiveSession(null)
          }
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
    <div className="dashboard" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)' }}>
      {isAdmin === false && (
        <div className="admin-banner glass-panel" style={{ display: 'flex', alignItems: 'center', gap: '16px', padding: '16px 24px', borderLeft: '4px solid var(--alert-red)', background: 'rgba(239, 68, 68, 0.05)' }}>
          <ShieldAlert size={24} color="var(--alert-red)" />
          <div className="admin-banner-text" style={{ flex: 1, fontSize: '0.9rem' }}>
            <strong style={{ color: 'var(--alert-red)' }}>Yönetici İzni Yok</strong> — Sürücüler listelenebilir ancak sektör tabanlı işlemler için uygulamayı <em>Yönetici Olarak Çalıştır</em> ile yeniden başlatmanız gerekir.
          </div>
        </div>
      )}

      <div className="glass-panel" role="note" style={{ padding: '12px 24px', color: 'var(--text-muted)', fontSize: '0.85rem' }}>
        Kanıt diski: motor GENERIC_READ. Windows birimi açıksa paylaşım FILE_SHARE_WRITE gerekebilir — donanım yazma koruması yoksa host OS kanıtı değiştirebilir. BitLocker: 64 hex FVEK sektör okumasını AES-128-XTS çözer; şifre/48-digit kırma yok. İmaj ham ciphertext yazar. PhysicalDrive imhası Yok Edici’de seri + onay ile.
      </div>

      <div className="glass-panel" style={{ padding: '16px 24px', display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center' }}>
        <label htmlFor="fvek-hex" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>BitLocker FVEK (64 hex)</label>
        <input
          id="fvek-hex"
          aria-label="BitLocker FVEK hex"
          value={fvekHex}
          onChange={(e) => { setFvekHex(e.target.value); setFvekStatus(null) }}
          placeholder="boş = anahtarı temizle"
          spellCheck={false}
          style={{ flex: 1, minWidth: '220px', padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)', fontFamily: 'monospace' }}
        />
        <button
          type="button"
          className="btn-secondary"
          onClick={async () => {
            if (!window.api?.setBitLockerFvek) {
              setFvekStatus('API yok')
              return
            }
            const hex = fvekHex.replace(/\s/g, '')
            const ok = await window.api.setBitLockerFvek(hex)
            setFvekStatus(ok ? (hex ? 'FVEK tarama/kurtarma okumasına uygulandı' : 'FVEK temizlendi') : 'Geçersiz (64 hex) veya motor hatası')
          }}
        >
          Uygula
        </button>
        {fvekStatus && <span style={{ fontSize: '0.85rem' }}>{fvekStatus}</span>}
      </div>

      {activeSession && (
        <div className="resume-banner glass-panel" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '16px 24px', borderLeft: '4px solid var(--accent-blue)', background: 'rgba(59, 130, 246, 0.05)' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
            <Activity size={24} color="var(--accent-blue)" className="spinner" />
            <div>
              <h3 style={{ fontSize: '1rem', marginBottom: '4px' }}>Devam Eden Tarama Bulundu</h3>
              <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>Sürücü {activeSession.driveIndex} üzerinde {activeSession.scanType} taraması yapılıyor. ({activeSession.scannedSectors} / {activeSession.totalSectors} sektör)</p>
            </div>
          </div>
          <button className="btn-primary" onClick={() => onAction && onAction('scan', { driveIndex: activeSession.driveIndex, scanType: activeSession.scanType })}>
            <Play size={16} fill="currentColor" /> Taramaya Dön
          </button>
        </div>
      )}

      <div className="dashboard-header" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-end', marginTop: '16px' }}>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Sistemdeki Sürücüler</h2>
          <p style={{ color: 'var(--text-muted)', fontSize: '0.95rem' }}>Erişilebilir tüm fiziksel donanımlar</p>
        </div>
        <button className="btn-secondary" onClick={fetchDrives} disabled={loading} style={{ display: 'flex', gap: '8px' }}>
          <RefreshCw size={16} className={loading ? 'spinner' : ''} /> Yenile
        </button>
      </div>

      {loading ? (
        <div className="loading-state glass-panel" style={{ padding: '80px', textAlign: 'center' }}>
          <RefreshCw size={32} className="spinner" style={{ margin: '0 auto 16px', color: 'var(--accent-blue)' }} />
          <p style={{ color: 'var(--text-muted)' }}>Sürücüler donanım seviyesinde sorgulanıyor...</p>
        </div>
      ) : drives.length === 0 ? (
        <div className="empty-state glass-panel" style={{ padding: '80px', textAlign: 'center' }}>
          <HardDrive size={48} style={{ margin: '0 auto 16px', color: 'var(--panel-border)' }} />
          <h3 style={{ marginBottom: '8px', fontSize: '1.2rem' }}>Sürücü Bulunamadı</h3>
          <p style={{ color: 'var(--text-muted)', marginBottom: '24px', maxWidth: '400px', margin: '0 auto 24px', lineHeight: 1.5 }}>
            {error || 'Sisteminizde desteklenen fiziksel bir disk tespit edilemedi. Lütfen bağlantıları kontrol edin.'}
          </p>
          <button className="btn-secondary" onClick={fetchDrives} style={{ display: 'inline-flex', gap: '8px' }}>
            <RotateCw size={16} /> Tekrar Dene
          </button>
        </div>
      ) : (
        <div className="drive-grid" style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(320px, 1fr))', gap: '24px' }}>
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

      <div className="dashboard-stats" style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: '24px', marginTop: '32px' }}>
        <div className="stat-card glass-panel" style={{ padding: '24px', display: 'flex', alignItems: 'center', gap: '16px' }}>
          <div style={{ background: 'rgba(255,255,255,0.05)', padding: '16px', borderRadius: '12px' }}><HardDrive size={28} color="var(--text-main)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{drives.length}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Bulunan Disk</div>
          </div>
        </div>
        <div className="stat-card glass-panel" style={{ padding: '24px', display: 'flex', alignItems: 'center', gap: '16px' }}>
          <div style={{ background: 'rgba(59, 130, 246, 0.1)', padding: '16px', borderRadius: '12px' }}><Activity size={28} color="var(--accent-blue)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{activeSession ? '1' : '0'}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Aktif Görev</div>
          </div>
        </div>
        <div className="stat-card glass-panel" style={{ padding: '24px', display: 'flex', alignItems: 'center', gap: '16px' }}>
          <div style={{ background: 'rgba(16, 185, 129, 0.1)', padding: '16px', borderRadius: '12px' }}><FolderCheck size={28} color="var(--success-green)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{latestScan?.recoveredFiles ?? 0}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Kurtarılan Dosya</div>
          </div>
        </div>
      </div>
    </div>
  )
}

export default Dashboard
