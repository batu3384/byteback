import React, { useEffect, useState } from 'react'
import './SmartView.css'

interface SmartViewProps {
  driveIndex?: number | null
}

function SmartView({ driveIndex }: SmartViewProps): React.ReactElement {
  const [smartData, setSmartData] = useState<any>(null)
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
      <div className="smart-view empty">
        <div className="icon-large">🩺</div>
        <h2>SMART Analizi</h2>
        <p>Lütfen Ana Ekran'dan bir sürücü seçin.</p>
      </div>
    )
  }

  return (
    <div className="smart-view">
      <div className="smart-header glass-panel">
        <div className="smart-header-info">
          <h2>Fiziksel Sürücü {driveIndex} SMART Verileri</h2>
          <p>Diskinizin donanımsal sağlık durumunu gösterir.</p>
        </div>
        <button className="btn-secondary" onClick={() => fetchSmartData(driveIndex)}>
          ↻ Yenile
        </button>
      </div>

      {loading ? (
        <div className="loading-state">
          <div className="spinner"></div>
          <p>SMART verileri okunuyor...</p>
        </div>
      ) : smartData && smartData.isValid ? (
        <div className="smart-content glass-panel">
          <div className="status-indicator good">
            <span className="status-dot"></span> Sürücü Sağlıklı
          </div>
          <table className="smart-table">
            <thead>
              <tr>
                <th>ID</th>
                <th>Özellik Adı</th>
                <th>Mevcut Değer</th>
                <th>En Kötü</th>
                <th>Eşik</th>
                <th>Durum</th>
              </tr>
            </thead>
            <tbody>
              <tr>
                <td colSpan={6} className="text-center">Ham veriler C++ modülünden henüz tam formatlanmamış olabilir. Yakında eklenecek.</td>
              </tr>
            </tbody>
          </table>
        </div>
      ) : (
        <div className="smart-content empty glass-panel">
          <h3>SMART Verisi Bulunamadı</h3>
          <p>Bu disk SMART analizini desteklemiyor veya veri okunamadı.</p>
        </div>
      )}
    </div>
  )
}

export default SmartView
