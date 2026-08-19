import React, { useEffect, useState } from 'react'
import './SmartView.css'
import { Activity, HardDrive, Thermometer, Clock, AlertTriangle, ShieldCheck, RefreshCw, Zap, Database, Power } from 'lucide-react'
import type { SmartStatus } from '../../../shared/types'

interface SmartViewProps {
  driveIndex?: number | null
}

function formatBytes(bytes: number): string {
  if (!bytes || bytes <= 0) return '—'
  if (bytes < 1024) return bytes + ' B'
  if (bytes < 1024 ** 2) return (bytes / 1024).toFixed(2) + ' KB'
  if (bytes < 1024 ** 4) return (bytes / 1024 ** 3).toFixed(2) + ' GB'
  return (bytes / 1024 ** 4).toFixed(2) + ' TB'
}

function SmartView({ driveIndex }: SmartViewProps): React.ReactElement {
  const [smartData, setSmartData] = useState<SmartStatus | null>(null)
  const [loading, setLoading] = useState<boolean>(false)

  useEffect(() => {
    if (driveIndex !== undefined && driveIndex !== null) {
      fetchSmartData(driveIndex)
    }
  }, [driveIndex])

  const fetchSmartData = async (index: number) => {
    setLoading(true)
    try {
      if (window.api && window.api.getSmartStatus) {
        const data = await window.api.getSmartStatus(index)
        setSmartData(data)
      }
    } catch (err) {
      console.error(err)
    } finally {
      setLoading(false)
    }
  }

  if (driveIndex === undefined || driveIndex === null) {
    return (
      <div className="smart-view empty glass-panel" style={{ padding: '60px', textAlign: 'center', margin: '40px' }}>
        <Activity size={48} style={{ margin: '0 auto 16px', color: 'var(--panel-border)' }} />
        <h3 style={{ fontSize: '1.2rem', marginBottom: '8px' }}>Sürücü Seçilmedi</h3>
        <p style={{ color: 'var(--text-muted)' }}>S.M.A.R.T. analizini görüntülemek için Ana Ekran'dan bir sürücü seçin.</p>
      </div>
    )
  }

  // Native engine reports "Good" / "Warning" / "Bad".
  const isHealthy = smartData && smartData.healthScore === 'Good'
  const hasWarnings = smartData && ((smartData.reallocatedSectors ?? 0) > 0 || (smartData.pendingSectors ?? 0) > 0)

  return (
    <div className="smart-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%', maxWidth: '900px', margin: '0 auto' }}>
      <div className="smart-header glass-panel" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '24px' }}>
        <div className="smart-header-info" style={{ display: 'flex', gap: '16px', alignItems: 'center' }}>
          <div style={{ background: 'rgba(59, 130, 246, 0.1)', padding: '16px', borderRadius: '12px' }}>
            <Activity size={32} color="var(--accent-blue)" />
          </div>
          <div>
            <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Donanım Sağlığı (S.M.A.R.T.)</h2>
            <p style={{ color: 'var(--text-muted)' }}>Fiziksel Sürücü {driveIndex} için doğrudan disk denetleyicisinden alınan veriler.</p>
          </div>
        </div>
        <button className="btn-secondary" onClick={() => fetchSmartData(driveIndex)} disabled={loading} style={{ display: 'flex', gap: '8px' }}>
          <RefreshCw size={16} className={loading ? 'spinner' : ''} /> Yenile
        </button>
      </div>

      {loading ? (
        <div className="loading-state glass-panel" style={{ padding: '60px', textAlign: 'center' }}>
          <RefreshCw size={32} className="spinner" style={{ margin: '0 auto 16px', color: 'var(--accent-blue)' }} />
          <p style={{ color: 'var(--text-muted)' }}>S.M.A.R.T. verileri okunuyor...</p>
        </div>
      ) : smartData && smartData.isValid ? (
        <div className="smart-content" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-md)' }}>

          <div className="smart-overview glass-panel" style={{
            padding: '24px', display: 'flex', alignItems: 'center', gap: '16px',
            borderLeft: `4px solid ${isHealthy && !hasWarnings ? 'var(--success-green)' : hasWarnings ? 'var(--warning-yellow)' : 'var(--alert-red)'}`
          }}>
            {isHealthy && !hasWarnings ? <ShieldCheck size={32} color="var(--success-green)" /> : <AlertTriangle size={32} color={hasWarnings ? 'var(--warning-yellow)' : 'var(--alert-red)'} />}
            <div style={{ flex: 1 }}>
              <h3 style={{ fontSize: '1.2rem', marginBottom: '4px' }}>
                {isHealthy && !hasWarnings ? 'Sürücü Sağlıklı' : hasWarnings ? 'Uyarı: Potansiyel Risk' : 'Kritik: Arıza Riski'}
              </h3>
              <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>
                Model: {smartData.driveModel || 'Bilinmiyor'}
                {smartData.isNvme ? ' · NVMe' : ''}
                {smartData.isSsd ? ' · SSD' : ''}
              </p>
            </div>
            <div style={{ fontSize: '2rem', fontWeight: 700, color: isHealthy && !hasWarnings ? 'var(--success-green)' : hasWarnings ? 'var(--warning-yellow)' : 'var(--alert-red)' }}>
              {smartData.healthScore || 'N/A'}
            </div>
          </div>
          {!smartData.isNvme && (
            <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem', marginTop: '-8px' }}>
              ATA sağlık skoru KALİBRASYONSUZ heuristic (η=500, β=1.5). Ömür tahmini değil; triyaj eşiği.
            </p>
          )}

          {(smartData.criticalWarning ?? 0) !== 0 && (
            <div className="glass-panel" style={{ padding: '16px 24px', borderLeft: '4px solid var(--alert-red)', background: 'rgba(239, 68, 68, 0.05)' }}>
              <div style={{ display: 'flex', gap: '12px', alignItems: 'center' }}>
                <AlertTriangle size={20} color="var(--alert-red)" />
                <div>
                  <strong style={{ color: 'var(--alert-red)' }}>Kritik Uyarı Bayrağı Aktif (0x{smartData.criticalWarning!.toString(16)})</strong>
                  <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>
                    Denetleyici kritik bir durum bildiriyor (salt-okunur mod, yedek pil hatası veya medya bozulması). Verileri HEMEN yedekleyin.
                  </p>
                </div>
              </div>
            </div>
          )}

          {smartData.isSsd && (
            <div className="glass-panel" style={{ padding: '16px 24px', borderLeft: '4px solid var(--warning-yellow)', background: 'rgba(245, 158, 11, 0.05)' }}>
              <div style={{ display: 'flex', gap: '12px', alignItems: 'flex-start' }}>
                <Zap size={20} color="var(--warning-yellow)" style={{ flexShrink: 0, marginTop: 2 }} />
                <div>
                  <strong style={{ color: 'var(--warning-yellow)' }}>SSD / TRIM Uyarısı</strong>
                  <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>
                    Bu sürücü SSD olarak tespit edildi. TRIM komutu, silinen verileri arka planda fiziksel olarak temizler —
                    silinmiş dosyalar SSD'den kurtarılamayabilir. Kritik veriler için anlık imaj almak (E01) en güvenli yaklaşımdır.
                  </p>
                </div>
              </div>
            </div>
          )}

          <div className="smart-grid" style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 1fr))', gap: 'var(--space-md)' }}>

            <div className="smart-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-muted)' }}>
                <Thermometer size={18} /> Sıcaklık
              </div>
              <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>
                {smartData.temperatureC ?? 0}°C
              </div>
              <div style={{ fontSize: '0.8rem', color: (smartData.temperatureC ?? 0) > 50 ? 'var(--warning-yellow)' : 'var(--success-green)' }}>
                {(smartData.temperatureC ?? 0) > 50 ? 'Yüksek Sıcaklık' : 'Normal Aralık'}
              </div>
            </div>

            <div className="smart-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-muted)' }}>
                <Clock size={18} /> Çalışma Süresi (POH)
              </div>
              <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>
                {smartData.powerOnHours}
              </div>
              <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)' }}>
                Saat ({Math.floor((smartData.powerOnHours ?? 0) / 24)} gün)
              </div>
            </div>

            <div className="smart-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-muted)' }}>
                <HardDrive size={18} /> Reallocated Sektör
              </div>
              <div style={{ fontSize: '1.8rem', fontWeight: 600, color: (smartData.reallocatedSectors ?? 0) > 0 ? 'var(--warning-yellow)' : 'inherit' }}>
                {smartData.reallocatedSectors}
              </div>
              <div style={{ fontSize: '0.8rem', color: (smartData.reallocatedSectors ?? 0) > 0 ? 'var(--warning-yellow)' : 'var(--success-green)' }}>
                {(smartData.reallocatedSectors ?? 0) > 0 ? 'Fiziksel hasar başlangıcı' : 'Sorun yok'}
              </div>
            </div>

            <div className="smart-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-muted)' }}>
                <AlertTriangle size={18} /> Bekleyen Sektör (Pending)
              </div>
              <div style={{ fontSize: '1.8rem', fontWeight: 600, color: (smartData.pendingSectors ?? 0) > 0 ? 'var(--alert-red)' : 'inherit' }}>
                {smartData.pendingSectors}
              </div>
              <div style={{ fontSize: '0.8rem', color: (smartData.pendingSectors ?? 0) > 0 ? 'var(--alert-red)' : 'var(--success-green)' }}>
                {(smartData.pendingSectors ?? 0) > 0 ? 'Okunamayan sektörler var' : 'Sorun yok'}
              </div>
            </div>

            {(smartData.percentageUsed ?? -1) >= 0 && (
              <div className="smart-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-muted)' }}>
                  <Zap size={18} /> Dayanıklılık Kullanımı
                </div>
                <div style={{ fontSize: '1.8rem', fontWeight: 600, color: (smartData.percentageUsed ?? 0) > 90 ? 'var(--warning-yellow)' : 'inherit' }}>
                  %{smartData.percentageUsed}
                </div>
                <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)' }}>
                  Yedek: {smartData.availableSpare !== undefined && smartData.availableSpare >= 0 ? `%${smartData.availableSpare}` : '—'}
                </div>
              </div>
            )}

            {(smartData.totalBytesWritten ?? 0) > 0 && (
              <div className="smart-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-muted)' }}>
                  <Database size={18} /> Toplam Yazma (TBW)
                </div>
                <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>
                  {formatBytes(smartData.totalBytesWritten!)}
                </div>
                <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)' }}>
                  Sürücü ömrüne yazılan veri
                </div>
              </div>
            )}

            {(smartData.unsafeShutdowns ?? 0) > 0 && (
              <div className="smart-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-muted)' }}>
                  <Power size={18} /> Güvensiz Kapanma
                </div>
                <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>
                  {smartData.unsafeShutdowns}
                </div>
                <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)' }}>
                  Ani güç kesintisi sayısı
                </div>
              </div>
            )}

          </div>
        </div>
      ) : (
        <div className="smart-content empty glass-panel" style={{ padding: '60px', textAlign: 'center' }}>
          <AlertTriangle size={48} style={{ margin: '0 auto 16px', color: 'var(--warning-yellow)' }} />
          <h3 style={{ fontSize: '1.2rem', marginBottom: '8px' }}>S.M.A.R.T. Verisi Okunamadı</h3>
          <p style={{ color: 'var(--text-muted)' }}>Bu disk S.M.A.R.T. analizini desteklemiyor veya NVMe/USB üzerinden doğrudan donanım erişimi kısıtlı olabilir.</p>
        </div>
      )}
    </div>
  )
}

export default SmartView
