import React, { useState } from 'react';
import './RaidBuilder.css';

interface Disk {
  id: string;
  name: string;
  capacity: string;
}

const RaidBuilder: React.FC = () => {
  const [availableDisks, setAvailableDisks] = useState<Disk[]>([
    { id: 'disk1', name: 'PhysicalDrive1 (WD Blue)', capacity: '1 TB' },
    { id: 'disk2', name: 'PhysicalDrive2 (Samsung EVO)', capacity: '1 TB' },
    { id: 'disk3', name: 'PhysicalDrive3 (Seagate Barracuda)', capacity: '1 TB' }
  ]);
  const [raidArray, setRaidArray] = useState<Disk[]>([]);
  const [raidType, setRaidType] = useState<string>('RAID 5');
  const [isBuilding, setIsBuilding] = useState(false);

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

  const buildRaid = () => {
    setIsBuilding(true);
    setTimeout(() => {
      setIsBuilding(false);
      alert(`${raidType} Volume Successfully Mounted! You can now scan it like a normal disk.`);
    }, 2000);
  };

  return (
    <div className="raid-builder-container">
      <div className="raid-header">
        <h2>Virtual RAID Constructor</h2>
        <p>Drag physical drives to form a Virtual RAID Volume (Software XOR/Striping).</p>
      </div>

      <div className="raid-workspace">
        {/* Available Disks Column */}
        <div 
          className="raid-column glass-panel available-column"
          onDragOver={(e) => e.preventDefault()}
          onDrop={(e) => handleDrop(e, 'available')}
        >
          <h3>Available Disks</h3>
          <div className="disk-list">
            {availableDisks.map(disk => (
              <div 
                key={disk.id} 
                className="disk-item" 
                draggable 
                onDragStart={(e) => handleDragStart(e, disk.id, 'available')}
              >
                <span className="disk-icon">💿</span>
                <div className="disk-info">
                  <span className="disk-name">{disk.name}</span>
                  <span className="disk-capacity">{disk.capacity}</span>
                </div>
              </div>
            ))}
            {availableDisks.length === 0 && <p className="empty-state">No available disks.</p>}
          </div>
        </div>

        {/* RAID Array Column */}
        <div 
          className="raid-column glass-panel array-column"
          onDragOver={(e) => e.preventDefault()}
          onDrop={(e) => handleDrop(e, 'array')}
        >
          <div className="array-header">
            <h3>Virtual Array</h3>
            <select className="raid-type-select" value={raidType} onChange={(e) => setRaidType(e.target.value)}>
              <option value="RAID 0">RAID 0 (Stripe)</option>
              <option value="RAID 1">RAID 1 (Mirror)</option>
              <option value="RAID 5">RAID 5 (Parity)</option>
            </select>
          </div>
          <div className="disk-list raid-slots">
            {raidArray.map((disk, index) => (
              <div 
                key={disk.id} 
                className="disk-item in-array" 
                draggable 
                onDragStart={(e) => handleDragStart(e, disk.id, 'array')}
              >
                <span className="disk-slot-number">Slot {index}</span>
                <span className="disk-icon">💿</span>
                <div className="disk-info">
                  <span className="disk-name">{disk.name}</span>
                </div>
              </div>
            ))}
            {raidArray.length === 0 && <p className="empty-state">Drag disks here to build array.</p>}
          </div>

          <button 
            className="btn-primary build-btn" 
            disabled={raidArray.length < 2 || isBuilding}
            onClick={buildRaid}
          >
            {isBuilding ? 'Constructing Parity...' : `MOUNT ${raidType} VOLUME`}
          </button>
        </div>
      </div>
    </div>
  );
};

export default RaidBuilder;
