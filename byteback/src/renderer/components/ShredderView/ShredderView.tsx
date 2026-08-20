import React, { useState } from 'react';
import './ShredderView.css';
import { AlertTriangle, ShieldAlert, CheckCircle, FileWarning } from 'lucide-react';

const ShredderView: React.FC = () => {
  const [status, setStatus] = useState<'idle' | 'shredding' | 'done' | 'failed'>('idle')
  const [ssdWarning, setSsdWarning] = useState(false)
  const [drives, setDrives] = useState<{ index: number; model?: string; serial?: string; type?: string; sizeBytes?: number }[]>([])
  const [wipeIndex, setWipeIndex] = useState(0)
  const [typedSerial, setTypedSerial] = useState('')
  const [confirmPhrase, setConfirmPhrase] = useState('')

  React.useEffect(() => {
    if (!window.api?.listDrives) return
    void window.api.listDrives().then((list) => {
      setDrives(list)
      setSsdWarning(list.some((d: { type?: string }) => d.type === 'SSD'))
    }).catch(() => undefined)
  }, [])

  const handleFreeSpaceWipe = async () => {
    if (!window.api?.pickAndWipeFreeSpace) return
    setStatus('shredding')
    const ok = await window.api.pickAndWipeFreeSpace()
    setStatus(ok ? 'done' : 'failed')
  }

  const handleFileWipe = async () => {
    if (!window.api?.pickAndWipeFile) return
    setStatus('shredding')
    const ok = await window.api.pickAndWipeFile()
    setStatus(ok ? 'done' : 'failed')
  }

  const handlePhysicalWipe = async () => {
    if (!window.api?.wipePhysicalDrive || !typedSerial.trim() || confirmPhrase !== 'IMHA') return
    setStatus('shredding')
    const ok = await window.api.wipePhysicalDrive(wipeIndex, typedSerial, confirmPhrase)
    setStatus(ok ? 'done' : 'failed')
  }

  const selectedDrive = drives.find((d) => d.index === wipeIndex)

  return (
    <div className="shredder-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%', maxWidth: '800px', margin: '0 auto' }}>
      <div className="shredder-header glass-panel" style={{ padding: '24px', display: 'flex', gap: '16px', alignItems: 'center', borderLeft: '4px solid var(--alert-red)' }}>
        <div style={{ background: 'rgba(239, 68, 68, 0.1)', padding: '16px', borderRadius: '12px' }}>
          <ShieldAlert size={32} color="var(--alert-red)" />
        </div>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px', color: 'var(--alert-red)' }}>Veri Yok Edici</h2>
          <p style={{ color: 'var(--text-muted)' }}>Dosya ve boş alan: DoD 3 geçiş. PhysicalDrive: seri eşleşmesi + OS onayından sonra tüm disk.</p>
        </div>
      </div>

      <div className="shredder-content" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
        <div className="shredder-warning glass-panel" style={{ padding: '20px', background: 'rgba(239, 68, 68, 0.05)', display: 'flex', gap: '16px', alignItems: 'flex-start' }}>
          <AlertTriangle size={24} color="var(--warning-yellow)" style={{ flexShrink: 0, marginTop: '2px' }} />
          <div>
            <h4 style={{ color: 'var(--warning-yellow)', marginBottom: '8px', fontSize: '1.1rem' }}>Sınırlar</h4>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem', lineHeight: 1.6 }}>
              Boş alan: tahsisli dosyalar ve file slack dokunulmaz. SSD/wear-leveling üzerinde DoD 5220.22-M NIST 800-88 sanitization değildir
              {ssdWarning ? ' — sistemde SSD görünüyor.' : '.'} PhysicalDrive imhası geri alınamaz.
            </p>
          </div>
        </div>

        <div className="shredder-card glass-panel" style={{ padding: '32px', display: 'flex', flexDirection: 'column', gap: '16px' }}>
          {status === 'idle' && (
            <>
              <button
                className="btn-danger shred-btn"
                onClick={() => void handleFreeSpaceWipe()}
                style={{ padding: '16px', fontSize: '1.1rem', fontWeight: 600, display: 'flex', justifyContent: 'center', alignItems: 'center', gap: '12px' }}
              >
                <ShieldAlert size={20} /> Boş alan imhası (klasör seç)
              </button>
              <button
                type="button"
                className="btn-secondary"
                onClick={() => void handleFileWipe()}
                style={{ padding: '12px', display: 'flex', justifyContent: 'center', alignItems: 'center', gap: '8px' }}
              >
                <FileWarning size={18} /> Dosya imha et
              </button>
              <label style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>PhysicalDrive imhası</label>
              <select
                aria-label="İmha sürücüsü"
                value={wipeIndex}
                onChange={(e) => setWipeIndex(Number(e.target.value))}
                style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
              >
                {drives.map((d) => (
                  <option key={d.index} value={d.index}>
                    {d.index}: {d.model || 'disk'} ({d.serial || 'seri yok'})
                  </option>
                ))}
              </select>
              <input
                aria-label="Disk seri numarası"
                placeholder="Listedeki seriyi buraya yazın"
                value={typedSerial}
                onChange={(e) => setTypedSerial(e.target.value)}
                style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
              />
              <input
                aria-label="Onay için IMHA yazın"
                placeholder='Onay: "IMHA" yazın'
                value={confirmPhrase}
                onChange={(e) => setConfirmPhrase(e.target.value.toUpperCase())}
                style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
              />
              {selectedDrive && (
                <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>
                  Hedef: {selectedDrive.model || 'disk'} — {selectedDrive.serial || 'seri yok'} — {selectedDrive.type || 'Unknown'}
                </p>
              )}
              <button
                type="button"
                className="btn-danger"
                disabled={!typedSerial.trim() || confirmPhrase !== 'IMHA' || drives.length === 0}
                onClick={() => void handlePhysicalWipe()}
                style={{ padding: '12px' }}
              >
                PhysicalDrive imha (seri + IMHA + onay)
              </button>
            </>
          )}

          {status === 'shredding' && (
            <div className="shred-progress glass-panel" role="status" aria-busy="true" style={{ padding: '24px', border: '1px solid rgba(239, 68, 68, 0.2)' }}>
              <p style={{ color: 'var(--alert-red)' }}>İmha sürüyor. Motor geçiş yüzdesi yayınlamaz; bitince sonuç yazılır. İptal yok.</p>
            </div>
          )}

          {status === 'done' && (
            <div className="shred-success glass-panel" style={{ padding: '24px', textAlign: 'center' }}>
              <CheckCircle size={48} color="var(--success-green)" style={{ margin: '0 auto 16px' }} />
              <h3 style={{ color: 'var(--success-green)', marginBottom: '8px' }}>İmha tamamlandı</h3>
              <p style={{ color: 'var(--text-muted)', marginBottom: '16px' }}>Seçilen hedef üzerine yazıldı.</p>
              <button className="btn-secondary" onClick={() => setStatus('idle')}>Yeni işlem</button>
            </div>
          )}

          {status === 'failed' && (
            <div className="glass-panel" role="alert" style={{ padding: '24px', textAlign: 'center' }}>
              <h3 style={{ marginBottom: '8px' }}>İmha yapılmadı</h3>
              <p style={{ color: 'var(--text-muted)', marginBottom: '16px' }}>İptal, seri uyuşmazlığı veya yazma hatası.</p>
              <button className="btn-secondary" onClick={() => setStatus('idle')}>Geri</button>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}

export default ShredderView
