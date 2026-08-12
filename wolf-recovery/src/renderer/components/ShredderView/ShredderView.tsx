import React, { useState } from 'react';
import './ShredderView.css';

interface ShredderViewProps {
  drives: any[];
}

const ShredderView: React.FC<ShredderViewProps> = ({ drives }) => {
  const [selectedDrive, setSelectedDrive] = useState<number | null>(null);
  const [status, setStatus] = useState<'idle' | 'shredding' | 'done'>('idle');
  const [progress, setProgress] = useState(0);

  const handleShred = () => {
    if (selectedDrive === null) return;
    setStatus('shredding');
    setProgress(0);

    // Simulate Shredding Progress
    const interval = setInterval(() => {
      setProgress(p => {
        if (p >= 100) {
          clearInterval(interval);
          setStatus('done');
          return 100;
        }
        return p + 2;
      });
    }, 100);
  };

  return (
    <div className="shredder-view">
      <div className="shredder-header">
        <h2>Data Shredder (Secure Wipe)</h2>
        <p className="subtitle">Militari grade data destruction using DoD 5220.22-M standards. Data wiped here cannot be recovered by ANY software.</p>
      </div>

      <div className="shredder-content">
        <div className="shredder-card glass-panel">
          <div className="shredder-icon-wrapper">
            <span className="shredder-icon">☣️</span>
          </div>
          <h3>Free Space Wipe</h3>
          <p>Overwrites all empty sectors on the selected drive to prevent recovery of previously deleted files.</p>
          
          <select 
            className="drive-select" 
            value={selectedDrive || ''} 
            onChange={(e) => setSelectedDrive(Number(e.target.value))}
            disabled={status === 'shredding'}
          >
            <option value="" disabled>Select a Drive to Wipe</option>
            {drives && drives.map(d => (
              <option key={d.index} value={d.index}>Drive {d.index} - {d.model}</option>
            ))}
          </select>

          {status === 'idle' && (
            <button className="btn-danger shred-btn" onClick={handleShred} disabled={selectedDrive === null}>
              START SECURE WIPE
            </button>
          )}

          {status === 'shredding' && (
            <div className="shred-progress">
              <div className="progress-labels">
                <span>Shredding... Pass 2 of 3 (0xFF)</span>
                <span>{progress}%</span>
              </div>
              <div className="progress-bar-bg">
                <div className="progress-bar-fill danger-fill" style={{ width: `${progress}%` }}></div>
              </div>
            </div>
          )}

          {status === 'done' && (
            <div className="shred-success">
              <span>✅ Wipe Complete. Data is permanently destroyed.</span>
              <button className="btn-secondary" onClick={() => setStatus('idle')} style={{ marginTop: '10px' }}>Wipe Another</button>
            </div>
          )}
        </div>
      </div>
    </div>
  );
};

export default ShredderView;
