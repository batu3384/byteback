import React, { useEffect, useState } from 'react';
import './DiskMap.css';

interface DiskMapProps {
  totalSectors: number;
  currentSector: number;
  badSectors?: number[];
}

const DiskMapVisualizer: React.FC<DiskMapProps> = ({ totalSectors, currentSector, badSectors = [] }) => {
  const [blocks, setBlocks] = useState<number[]>([]);
  const MAX_BLOCKS = 400; // Generate a fixed 20x20 grid for visualization

  useEffect(() => {
    // Generate an array of block statuses
    // 0: Unscanned, 1: Scanned (Healthy), 2: Bad Sector
    const newBlocks = Array(MAX_BLOCKS).fill(0);
    const progressRatio = totalSectors > 0 ? (currentSector / totalSectors) : 0;
    const filledBlocks = Math.floor(progressRatio * MAX_BLOCKS);

    for (let i = 0; i < filledBlocks; i++) {
      newBlocks[i] = 1; // Healthy by default
    }

    if (badSectors.length > 0) {
      badSectors.forEach(bs => {
        const blockIndex = Math.floor((bs / totalSectors) * MAX_BLOCKS);
        if (blockIndex < MAX_BLOCKS && blockIndex < filledBlocks) {
          newBlocks[blockIndex] = 2; // Mark as Bad Sector
        }
      });
    }

    setBlocks(newBlocks);
  }, [currentSector, totalSectors, badSectors]);

  return (
    <div className="disk-map-container glass-panel">
      <div className="disk-map-header">
        <h3>Canlı Disk Yüzey Haritası</h3>
        <div className="disk-map-legend">
          <span className="legend-item"><span className="dot healthy"></span> Sağlıklı</span>
          <span className="legend-item"><span className="dot bad"></span> Bozuk sektör</span>
          <span className="legend-item"><span className="dot unscanned"></span> Taranmadı</span>
        </div>
      </div>
      
      <div className="disk-map-grid">
        {blocks.map((status, index) => {
          let className = 'disk-block';
          if (status === 1) className += ' block-healthy';
          if (status === 2) className += ' block-bad';
          
          const isScanning = status === 1 && (index === blocks.lastIndexOf(1) || index === blocks.indexOf(0) - 1);
          if (isScanning) className += ' block-scanning';

          return <div key={index} className={className}></div>;
        })}
      </div>
    </div>
  );
};

export default DiskMapVisualizer;
