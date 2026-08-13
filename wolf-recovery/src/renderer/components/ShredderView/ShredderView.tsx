import React, { useState, useRef } from 'react';
import './ShredderView.css';
import { Skull, AlertTriangle, ShieldAlert, CheckCircle, Server, FileWarning } from 'lucide-react';

interface ShredderViewProps {
  drives?: any[]; // optional now
}

const ShredderView: React.FC<ShredderViewProps> = ({ drives: initialDrives }) => {
  const [drives, setDrives] = useState<any[]>(initialDrives || []);
  const [selectedDrive, setSelectedDrive] = useState<number | null>(null);
  const [status, setStatus] = useState<'idle' | 'shredding' | 'done'>('idle');
  const [progress, setProgress] = useState(0);
  const progressTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);

  React.useEffect(() => {
    return () => {
      // Defensive cleanup on unmount.
      if (progressTimerRef.current) clearInterval(progressTimerRef.current);
    };
  }, []);

  const handleShred = () => {
    if (selectedDrive === null) return;

    if (!window.confirm(`DİKKAT! Sürücü ${selectedDrive} üzerindeki boş alanlar KALICI OLARAK silinecektir. Bu işlem GERİ ALINAMAZ. Onaylıyor musunuz?`)) {
      return;
    }

    // Set initial loading state
    setStatus('shredding');
    setProgress(5);

    // The native wipe is async and does not yet stream per-pass progress
    // (a structured progress event is on the roadmap). Until then we run an
    // indeterminate ticker so the UI reflects "work in progress" rather than
    // a frozen bar, capped below 95% so completion still visibly advances.
    if (progressTimerRef.current) clearInterval(progressTimerRef.current);
    progressTimerRef.current = setInterval(() => {
      setProgress(prev => (prev >= 95 ? prev : prev + 1));
    }, 800);

    // Call the real IPC endpoint
    if (window.api && window.api.startWipe) {
      window.api.startWipe(selectedDrive.toString()).then((success: boolean) => {
        if (progressTimerRef.current) {
          clearInterval(progressTimerRef.current);
          progressTimerRef.current = null;
        }
        if (success) {
          setProgress(100);
          setStatus('done');
        } else {
          alert('Silme işlemi başarısız oldu. Lütfen uygulamayı Yönetici (Administrator) olarak çalıştırdığınızdan emin olun.');
          setStatus('idle');
        }
      }).catch((e: any) => {
        if (progressTimerRef.current) {
          clearInterval(progressTimerRef.current);
          progressTimerRef.current = null;
        }
        console.error(e);
        alert('Silme işlemi sırasında hata oluştu.');
        setStatus('idle');
      });
    } else {
      if (progressTimerRef.current) {
        clearInterval(progressTimerRef.current);
        progressTimerRef.current = null;
      }
      alert('Kritik Hata: IPC (window.api) modülüne erişilemiyor. Lütfen uygulamayı tam yetkili modunda çalıştırın.');
      setStatus('idle');
    }
  };

  return (
    <div className="shredder-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%', maxWidth: '800px', margin: '0 auto' }}>
      <div className="shredder-header glass-panel" style={{ padding: '24px', display: 'flex', gap: '16px', alignItems: 'center', borderLeft: '4px solid var(--alert-red)' }}>
        <div style={{ background: 'rgba(239, 68, 68, 0.1)', padding: '16px', borderRadius: '12px' }}>
          <Skull size={32} color="var(--alert-red)" />
        </div>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px', color: 'var(--alert-red)' }}>Veri Yok Edici (Data Shredder)</h2>
          <p style={{ color: 'var(--text-muted)' }}>Milli Savunma (DoD 5220.22-M) standartlarında veri imhası. Bu araçla silinen veriler HİÇBİR YAZILIM tarafından geri getirilemez.</p>
        </div>
      </div>

      <div className="shredder-content" style={{ display: 'flex', flexDirection: 'column', gap: '24px' }}>
        
        <div className="shredder-warning glass-panel" style={{ padding: '20px', background: 'rgba(239, 68, 68, 0.05)', display: 'flex', gap: '16px', alignItems: 'flex-start' }}>
          <AlertTriangle size={24} color="var(--warning-yellow)" style={{ flexShrink: 0, marginTop: '2px' }} />
          <div>
            <h4 style={{ color: 'var(--warning-yellow)', marginBottom: '8px', fontSize: '1.1rem' }}>Önemli Güvenlik Uyarısı</h4>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem', lineHeight: 1.6 }}>
              Bu işlem, seçtiğiniz diskin <strong>sadece boş alanlarındaki</strong> (silinmiş ama fiziksel olarak diskte duran) kalıntıları tamamen yok eder. Mevcut dosyalarınıza zarar vermez ancak eskiden silinmiş verilerin kurtarılmasını imkansız hale getirir. SSD sürücülerde (TRIM) bu işlem diskin ömrünü (TBW) bir miktar azaltabilir.
            </p>
          </div>
        </div>

        <div className="shredder-card glass-panel" style={{ padding: '32px', display: 'flex', flexDirection: 'column', gap: '24px' }}>
          
          <div className="form-group" style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
            <label style={{ fontSize: '0.9rem', color: 'var(--text-muted)', fontWeight: 500, display: 'flex', alignItems: 'center', gap: '8px' }}>
              <Server size={16} /> Hedef Sürücü
            </label>
            <select 
              style={{ width: '100%', padding: '12px 16px', background: 'rgba(0,0,0,0.2)', border: '1px solid var(--panel-border)', borderRadius: '8px', color: 'var(--text-main)', fontSize: '1rem', outline: 'none' }}
              value={selectedDrive || ''} 
              onChange={(e) => setSelectedDrive(Number(e.target.value))}
              disabled={status === 'shredding'}
            >
              <option value="" disabled style={{ background: 'var(--bg-surface)' }}>Temizlenecek Sürücüyü Seçin...</option>
              {drives && drives.map(d => (
                <option key={d.index} value={d.index} style={{ background: 'var(--bg-surface)' }}>
                  Sürücü {d.index} - {d.model} ({Math.floor(d.sizeBytes / (1024*1024*1024))} GB)
                </option>
              ))}
            </select>
          </div>

          <div className="form-group" style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
            <label style={{ fontSize: '0.9rem', color: 'var(--text-muted)', fontWeight: 500 }}>Güvenlik Standardı</label>
            <select style={{ width: '100%', padding: '12px 16px', background: 'rgba(0,0,0,0.2)', border: '1px solid var(--panel-border)', borderRadius: '8px', color: 'var(--text-main)', fontSize: '1rem', outline: 'none' }} disabled>
              <option>US DoD 5220.22-M (3-Pass Wipe)</option>
            </select>
          </div>

          {status === 'idle' && (
            <button 
              className="btn-danger shred-btn" 
              onClick={handleShred} 
              disabled={selectedDrive === null}
              style={{ padding: '16px', fontSize: '1.1rem', fontWeight: 600, display: 'flex', justifyContent: 'center', alignItems: 'center', gap: '12px', marginTop: '16px' }}
            >
              <ShieldAlert size={20} /> GÜVENLİ İMHAYI BAŞLAT
            </button>
          )}

          {status === 'shredding' && (
            <div className="shred-progress glass-panel" style={{ padding: '24px', background: 'rgba(239, 68, 68, 0.02)', border: '1px solid rgba(239, 68, 68, 0.2)' }}>
              <div className="progress-labels" style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.9rem', color: 'var(--alert-red)', marginBottom: '8px' }}>
                <span className="pulsing-text">İmha Ediliyor... (Geçiş {progress < 33 ? '1' : progress < 66 ? '2' : '3'} / 3)</span>
                <span>{progress}%</span>
              </div>
              <div className="progress-bar-bg" style={{ width: '100%', height: '12px', background: 'rgba(0,0,0,0.3)', borderRadius: '6px', overflow: 'hidden' }}>
                <div className="progress-bar-fill" style={{ width: `${progress}%`, height: '100%', background: 'var(--alert-red)', transition: 'width 0.5s linear' }}></div>
              </div>
              <p style={{ fontSize: '0.8rem', color: 'var(--text-muted)', marginTop: '12px', textAlign: 'center' }}>Bu işlem diskinizin hızına bağlı olarak uzun sürebilir. Lütfen bekleyin.</p>
            </div>
          )}

          {status === 'done' && (
            <div className="shred-success glass-panel" style={{ padding: '24px', background: 'rgba(16, 185, 129, 0.05)', border: '1px solid rgba(16, 185, 129, 0.2)', textAlign: 'center' }}>
              <CheckCircle size={48} color="var(--success-green)" style={{ margin: '0 auto 16px' }} />
              <h3 style={{ color: 'var(--success-green)', marginBottom: '8px', fontSize: '1.2rem' }}>İmha İşlemi Tamamlandı</h3>
              <p style={{ color: 'var(--text-muted)', marginBottom: '16px' }}>Seçilen sürücüdeki boş alanlar güvenli bir şekilde (3 Pass) üzerine yazılarak imha edildi.</p>
              <button className="btn-secondary" onClick={() => setStatus('idle')}>Başka Bir Sürücü Temizle</button>
            </div>
          )}
        </div>
      </div>
    </div>
  );
};

export default ShredderView;
