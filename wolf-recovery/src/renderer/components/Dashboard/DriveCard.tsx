import React from 'react';
import type { DriveInfo } from '../../../shared/types';
import './DriveCard.css';

interface DriveCardProps {
  drive: DriveInfo;
}

function DriveCard({ drive }: DriveCardProps): React.ReactElement {
  const formatSize = (bytes: number): string => {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
  };

  const getDriveIcon = (type: string) => {
    switch (type) {
      case 'USB':
        return (
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <rect x="6" y="2" width="12" height="6" rx="1"></rect>
            <path d="M10 2v6"></path>
            <path d="M14 2v6"></path>
            <rect x="4" y="8" width="16" height="14" rx="2"></rect>
            <circle cx="12" cy="15" r="2"></circle>
          </svg>
        );
      case 'SSD':
        return (
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <rect x="2" y="4" width="20" height="16" rx="2" ry="2"></rect>
            <line x1="6" y1="8" x2="6.01" y2="8"></line>
            <line x1="10" y1="8" x2="10.01" y2="8"></line>
            <line x1="14" y1="8" x2="14.01" y2="8"></line>
            <line x1="18" y1="8" x2="18.01" y2="8"></line>
            <rect x="6" y="12" width="12" height="4"></rect>
          </svg>
        );
      case 'HDD':
      default:
        return (
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <ellipse cx="12" cy="5" rx="9" ry="3"></ellipse>
            <path d="M21 12c0 1.66-4 3-9 3s-9-1.34-9-3"></path>
            <path d="M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5"></path>
          </svg>
        );
    }
  };

  // Mock health for now (could be green, yellow, red)
  const healthClass = drive.type === 'USB' ? 'warning' : 'success';

  return (
    <div className="drive-card">
      <div className="drive-header">
        <div className="drive-icon-container">
          {getDriveIcon(drive.type)}
        </div>
        <div className="drive-info">
          <h3 className="drive-model">{drive.model || 'Unknown Drive'}</h3>
          <div className="drive-meta">
            <span className={`drive-type type-${drive.type.toLowerCase()}`}>
              {drive.type}
            </span>
            <span className="drive-index">Drive {drive.index}</span>
          </div>
        </div>
        <div className="drive-health" title={`Health: ${healthClass}`}>
          <div className={`health-indicator bg-${healthClass}`}></div>
        </div>
      </div>

      <div className="drive-details">
        <div className="detail-row">
          <span className="detail-label">Size</span>
          <span className="detail-value">{formatSize(drive.sizeBytes)}</span>
        </div>
        <div className="detail-row">
          <span className="detail-label">Sector Size</span>
          <span className="detail-value">{drive.sectorSize} B</span>
        </div>
        <div className="detail-row">
          <span className="detail-label">Serial</span>
          <span className="detail-value mono">{drive.serial || 'N/A'}</span>
        </div>
      </div>

      <div className="drive-actions">
        <button className="btn btn-secondary">Quick Scan</button>
        <button className="btn btn-primary">Deep Scan</button>
      </div>
    </div>
  );
}

export default DriveCard;
