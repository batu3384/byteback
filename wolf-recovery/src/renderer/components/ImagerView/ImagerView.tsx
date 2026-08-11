import React, { useState } from 'react'
import './ImagerView.css'

function ImagerView(): React.ReactElement {
  const [imaging, setImaging] = useState(false)
  const [progress, setProgress] = useState(0)

  const handleStartImaging = () => {
    setImaging(true)
    // Fake progress for UI presentation
    let current = 0
    const interval = setInterval(() => {
      current += 1
      setProgress(current)
      if (current >= 100) {
        clearInterval(interval)
        setImaging(false)
        alert('İmaj alma tamamlandı! (Simülasyon)')
      }
    }, 100)
  }

  return (
    <div className="imager-view">
      <div className="imager-header glass-panel">
        <h2>Disk İmaj Alma (Adli Kopyalama)</h2>
        <p>Seçilen diskin sektör-sektör birebir kopyasını oluşturun. Adli bilişim standartlarında imaj formatlarını destekler.</p>
      </div>

      <div className="imager-content glass-panel">
        <div className="form-group">
          <label>Kaynak Sürücü</label>
          <select className="form-select">
            <option>Seçiniz...</option>
            <option>Fiziksel Sürücü 0 (1 TB)</option>
            <option>Fiziksel Sürücü 1 (500 GB)</option>
          </select>
        </div>

        <div className="form-group">
          <label>İmaj Formatı</label>
          <select className="form-select">
            <option>RAW (DD) Birebir Kopya (.dd, .img)</option>
            <option>EnCase Forensic Format (.E01)</option>
            <option>Advanced Forensic Format (.aff4)</option>
          </select>
        </div>

        <div className="form-group">
          <label>Hedef Dizin</label>
          <div className="path-input-group">
            <input type="text" className="form-input" placeholder="C:\Images\" readOnly />
            <button className="btn-secondary">Gözat</button>
          </div>
        </div>

        <div className="form-actions">
          <button 
            className="btn-primary start-btn" 
            onClick={handleStartImaging}
            disabled={imaging}
          >
            {imaging ? 'İmaj Alınıyor...' : 'İmaj Almayı Başlat'}
          </button>
        </div>

        {imaging && (
          <div className="imager-progress">
            <div className="progress-bar-bg">
              <div className="progress-bar-fill" style={{ width: `${progress}%` }}></div>
            </div>
            <div className="progress-stats">
              <span>%{progress} Tamamlandı</span>
              <span>Kalan Süre: Hesaplanıyor...</span>
            </div>
          </div>
        )}
      </div>
    </div>
  )
}

export default ImagerView
