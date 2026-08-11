import React from 'react'
import './ImagerView.css'

function ImagerView(): React.ReactElement {
  return (
    <div className="imager-view">
      <div className="imager-header glass-panel">
        <h2>Disk İmaj Alma (Adli Kopyalama)</h2>
        <p>Seçilen diskin sektör-sektör birebir kopyasını oluşturun. Adli bilişim standartlarında imaj formatlarını destekler.</p>
      </div>

      <div className="imager-content glass-panel">
        <div className="coming-soon-container">
          <div className="coming-soon-icon">🔬</div>
          <h3>Geliştirme Aşamasında</h3>
          <p>Disk imaj alma özelliği şu anda aktif geliştirme aşamasındadır. Gelecek sürümde aşağıdaki özellikler desteklenecektir:</p>
          <ul className="feature-list">
            <li>RAW (DD) birebir disk kopyalama</li>
            <li>EnCase Forensic Format (.E01) desteği</li>
            <li>Bütünlük doğrulama (SHA-256 / MD5 hash)</li>
            <li>Sıkıştırılmış imaj formatları</li>
          </ul>
        </div>
      </div>
    </div>
  )
}

export default ImagerView
