import React, { useEffect, useState } from 'react'
import DriveCard from './DriveCard'
import type { DriveInfo } from '../../../shared/types'
import './Dashboard.css'

function Dashboard(): React.ReactElement {
  const [drives, setDrives] = useState<DriveInfo[]>([])
  const [loading, setLoading] = useState(true)

  const fetchDrives = () => {
    setLoading(true)
    if (window.api && window.api.listDrives) {
      window.api.listDrives()
        .then((result) => {
          setDrives(result as DriveInfo[])
        })
        .catch((err) => {
          console.error('Error fetching drives:', err)
          setDrives([])
        })
        .finally(() => {
          setLoading(false)
        })
    } else {
      console.warn('API listDrives not available')
      setDrives([])
      setLoading(false)
    }
  }

  useEffect(() => {
    fetchDrives()
  }, [])

  return (
    <div className="dashboard">
      <div className="dashboard-header">
        <h2>Connected Drives</h2>
        <button className="btn-refresh" onClick={fetchDrives}>
          ↻ Refresh
        </button>
      </div>

      {loading ? (
        <div className="loading-spinner">Scanning drives...</div>
      ) : drives.length === 0 ? (
        <div className="empty-state">No drives detected. Run as Administrator.</div>
      ) : (
        <div className="drive-grid">
          {drives.map((drive) => (
            <DriveCard key={drive.index} drive={drive} />
          ))}
        </div>
      )}

      <div className="dashboard-stats">
        <div className="stat-card">
          <span className="stat-value">{drives.length}</span>
          <span className="stat-label">Drives Detected</span>
        </div>
        <div className="stat-card">
          <span className="stat-value">0</span>
          <span className="stat-label">Active Scans</span>
        </div>
        <div className="stat-card">
          <span className="stat-value">0</span>
          <span className="stat-label">Files Recovered</span>
        </div>
      </div>
    </div>
  )
}

export default Dashboard
