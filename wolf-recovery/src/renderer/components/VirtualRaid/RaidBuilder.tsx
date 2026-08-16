import React, { useState, useEffect } from 'react';
import './RaidBuilder.css';
import { Layers, HardDrive, Cpu, Settings2, Play, CheckCircle } from 'lucide-react';

interface Disk {
  id: string;
  name: string;
  capacity: string;
}

const RaidBuilder: React.FC = () => {
  const [availableDisks, setAvailableDisks] = useState<Disk[]>([]);
  const [raidArray, setRaidArray] = useState<Disk[]>([]);
  const [raidType, setRaidType] = useState<string>('RAID 5');
  const [isBuilding, setIsBuilding] = useState(false);

  useEffect(() => {
    if (window.api && window.api.listDrives) {
      window.api.listDrives().then((drives: any[]) => {
        const disks = drives.map(d => ({
          id: d.index.toString(),
          name: `Sürücü ${d.index} (${d.model})`,
          capacity: `${(d.sizeBytes / (1024 ** 3)).toFixed(2)} GB`
        }));
        setAvailableDisks(disks);
      }).catch(console.error);
    }
  }, []);

  const handleDragStart = (e: React.DragEvent, diskId: string, from: 'available' | 'array') => {
    e.dataTransfer.setData('diskId', diskId);
    e.dataTransfer.setData('from', from);
  };

  const handleDrop = (e: React.DragEvent, to: 'available' | 'array') => {
    e.preventDefault();
    const diskId = e.dataTransfer.getData('diskId');
    const from = e.dataTransfer.getData('from');

    if (from === to) return;

    if (from === 'available' && to === 'array') {
      const disk = availableDisks.find(d => d.id === diskId);
      if (disk) {
        setAvailableDisks(availableDisks.filter(d => d.id !== diskId));
        setRaidArray([...raidArray, disk]);
      }
    } else if (from === 'array' && to === 'available') {
      const disk = raidArray.find(d => d.id === diskId);
      if (disk) {
        setRaidArray(raidArray.filter(d => d.id !== diskId));
        setAvailableDisks([...availableDisks, disk]);
      }
    }
  };

  const buildRaid = async () => {
    if (raidArray.length < 2) return;
    setIsBuilding(true);
    if (window.api && window.api.reconstructRaid) {
      // Map the UI selection to the numeric RaidLevel enum used by the native
      // engine (virtual_raid.h: RAID0=0, RAID1=1, RAID5=2, RAID6=3, RAID10=4).
      // Pass the drives in their user-ordered slot order, since stripe/parity
      // layout depends on it.
      const raidLevelMap: Record<string, number> = { 'RAID 0': 0, 'RAID 1': 1, 'RAID 5': 2, 'RAID 6': 3, 'RAID 10': 4 };
      const raidLevel = raidLevelMap[raidType] ?? 2;
      const driveIndices = raidArray.map(d => Number(d.id));
      const res = await window.api.reconstructRaid(driveIndices, raidLevel);
      setIsBuilding(false);
      if (res && res.success) {
        const capGb = res.capacity ? (res.capacity / (1024 ** 3)).toFixed(2) : '?';
        alert(`${raidType} dizisi başarıyla oluşturuldu!\n\nKapasite: ${capGb} GB\nDisk sayısı: ${res.numDisks}\n\nArtık bu diziyi normal bir disk gibi tarayabilirsiniz.`);
      } else {
        const why = res && res.error ? `\n\nHata: ${res.error}` : '';
        alert(`${raidType} dizisi oluşturulamadı. Disk sırasını, minimum disk sayısını (RAID 5: 3, RAID 6: 4, RAID 10: çift sayı) ve Yönetici izinlerini kontrol edin.${why}`);
      }
    } else {
      setIsBuilding(false);
      alert('Kritik Hata: IPC modülüne ulaşılamıyor (Native Backend aktif değil). Lütfen Yönetici olarak çalıştırın.');
    }
  };

  return (
    <div className="raid-builder-container" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%', maxWidth: '1000px', margin: '0 auto' }}>
      <div className="raid-header glass-panel" style={{ padding: '24px', display: 'flex', gap: '16px', alignItems: 'center' }}>
        <div style={{ background: 'rgba(183, 0, 255, 0.1)', padding: '16px', borderRadius: '12px' }}>
          <Layers size={32} color="#b700ff" />
        </div>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Sanal RAID Oluşturucu (Virtual RAID Constructor)</h2>
          <p style={{ color: 'var(--text-muted)' }}>Fiziksel sürücüleri sürükleyip bırakarak Sanal RAID Diski oluşturun. Bozulan RAID dizilerinden (Yazılımsal XOR/Striping) veri kurtarmayı sağlar.</p>
        </div>
      </div>

      <div className="raid-workspace" style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '24px', flex: 1, minHeight: 0 }}>
        
        {/* Available Disks Column */}
        <div 
          className="raid-column glass-panel available-column"
          onDragOver={(e) => e.preventDefault()}
          onDrop={(e) => handleDrop(e, 'available')}
          style={{ display: 'flex', flexDirection: 'column', overflow: 'hidden' }}
        >
          <div style={{ padding: '20px 24px', borderBottom: '1px solid var(--panel-border)', display: 'flex', alignItems: 'center', gap: '12px' }}>
            <HardDrive size={20} color="var(--accent-blue)" />
            <h3 style={{ fontSize: '1.1rem', margin: 0 }}>Kullanılabilir Diskler</h3>
          </div>
          <div className="disk-list" style={{ padding: '24px', flex: 1, overflowY: 'auto', display: 'flex', flexDirection: 'column', gap: '12px' }}>
            {availableDisks.map(disk => (
              <div 
                key={disk.id} 
                className="disk-item" 
                draggable 
                onDragStart={(e) => handleDragStart(e, disk.id, 'available')}
                style={{ 
                  padding: '16px', background: 'rgba(255,255,255,0.03)', border: '1px solid var(--panel-border)', 
                  borderRadius: '8px', cursor: 'grab', display: 'flex', alignItems: 'center', gap: '16px',
                  transition: 'var(--transition-smooth)'
                }}
                onMouseEnter={(e) => e.currentTarget.style.background = 'rgba(255,255,255,0.08)'}
                onMouseLeave={(e) => e.currentTarget.style.background = 'rgba(255,255,255,0.03)'}
              >
                <HardDrive size={24} color="var(--text-muted)" />
                <div className="disk-info" style={{ flex: 1 }}>
                  <div className="disk-name" style={{ fontWeight: 500, color: 'var(--text-main)', marginBottom: '4px' }}>{disk.name}</div>
                  <div className="disk-capacity" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>{disk.capacity}</div>
                </div>
              </div>
            ))}
            {availableDisks.length === 0 && (
              <div className="empty-state" style={{ textAlign: 'center', color: 'var(--text-muted)', margin: 'auto', padding: '40px 0' }}>
                <HardDrive size={48} style={{ margin: '0 auto 16px', opacity: 0.2 }} />
                <p>Burada disk bulunmuyor.</p>
              </div>
            )}
          </div>
        </div>

        {/* RAID Array Column */}
        <div 
          className="raid-column glass-panel array-column"
          onDragOver={(e) => e.preventDefault()}
          onDrop={(e) => handleDrop(e, 'array')}
          style={{ display: 'flex', flexDirection: 'column', overflow: 'hidden', border: '1px solid #b700ff44', boxShadow: 'inset 0 0 40px rgba(183, 0, 255, 0.05)' }}
        >
          <div className="array-header" style={{ padding: '20px 24px', borderBottom: '1px solid var(--panel-border)', display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
              <Cpu size={20} color="#b700ff" />
              <h3 style={{ fontSize: '1.1rem', margin: 0 }}>Sanal Dizi (Array)</h3>
            </div>
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
              <Settings2 size={16} color="var(--text-muted)" />
              <select
                className="raid-type-select"
                value={raidType}
                onChange={(e) => setRaidType(e.target.value)}
                style={{ background: 'rgba(0,0,0,0.3)', border: '1px solid var(--panel-border)', color: 'var(--text-main)', padding: '6px 12px', borderRadius: '6px', outline: 'none' }}
              >
                <option value="RAID 0">RAID 0 (Stripe)</option>
                <option value="RAID 1">RAID 1 (Mirror)</option>
                <option value="RAID 5">RAID 5 (Parity)</option>
                <option value="RAID 6">RAID 6 (Çift Parite)</option>
                <option value="RAID 10">RAID 10 (Mirror+Stripe)</option>
              </select>
            </div>
          </div>

          <div className="disk-list raid-slots" style={{ padding: '24px', flex: 1, overflowY: 'auto', display: 'flex', flexDirection: 'column', gap: '12px', background: 'rgba(183, 0, 255, 0.02)' }}>
            {raidArray.map((disk, index) => (
              <div 
                key={disk.id} 
                className="disk-item in-array" 
                draggable 
                onDragStart={(e) => handleDragStart(e, disk.id, 'array')}
                style={{ 
                  padding: '16px', background: 'rgba(183, 0, 255, 0.1)', border: '1px solid rgba(183, 0, 255, 0.3)', 
                  borderRadius: '8px', cursor: 'grab', display: 'flex', alignItems: 'center', gap: '16px'
                }}
              >
                <div style={{ background: '#b700ff', color: 'white', padding: '4px 8px', borderRadius: '4px', fontSize: '0.8rem', fontWeight: 600 }}>Slot {index}</div>
                <HardDrive size={24} color="white" />
                <div className="disk-info" style={{ flex: 1 }}>
                  <div className="disk-name" style={{ fontWeight: 500, color: 'white', marginBottom: '4px' }}>{disk.name}</div>
                  <div className="disk-capacity" style={{ fontSize: '0.85rem', color: 'rgba(255,255,255,0.7)' }}>{disk.capacity}</div>
                </div>
              </div>
            ))}
            {raidArray.length === 0 && (
              <div className="empty-state" style={{ textAlign: 'center', color: 'var(--text-muted)', margin: 'auto', padding: '40px 0' }}>
                <div style={{ padding: '20px', border: '2px dashed var(--panel-border)', borderRadius: '12px', display: 'inline-block', marginBottom: '16px' }}>
                  <HardDrive size={32} style={{ opacity: 0.5 }} />
                </div>
                <p>Diskleri bu alana sürükleyerek sıralayın.</p>
              </div>
            )}
          </div>

          <div style={{ padding: '24px', borderTop: '1px solid var(--panel-border)' }}>
            <button 
              className="btn-primary build-btn" 
              disabled={raidArray.length < 2 || isBuilding}
              onClick={buildRaid}
              style={{ width: '100%', padding: '16px', fontSize: '1.1rem', fontWeight: 600, display: 'flex', justifyContent: 'center', alignItems: 'center', gap: '12px', background: raidArray.length >= 2 ? '#b700ff' : 'var(--panel-border)', color: 'white' }}
            >
              {isBuilding ? (
                <>
                  <Settings2 size={20} className="spinner" /> Parite Hesaplanıyor...
                </>
              ) : (
                <>
                  <Play size={20} fill="currentColor" /> {raidType} DİZİSİNİ OLUŞTUR VE BAĞLA
                </>
              )}
            </button>
            <p style={{ textAlign: 'center', color: 'var(--text-muted)', fontSize: '0.85rem', marginTop: '12px' }}>
              En az 2 disk gereklidir. Disklerin dizilim sırası XOR paritesini doğrudan etkiler.
            </p>
          </div>
        </div>
      </div>
    </div>
  );
};

export default RaidBuilder;
