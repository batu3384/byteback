import React from 'react'
import { FolderSearch } from 'lucide-react'

interface ScanRequiredPanelProps {
  title?: string
  onGoDashboard?: () => void
}

export default function ScanRequiredPanel({
  title = 'Tamamlanmış tarama gerekli',
  onGoDashboard,
}: ScanRequiredPanelProps): React.ReactElement {
  return (
    <div
      className="scan-required-panel glass-panel"
      role="note"
      style={{
        maxWidth: '560px',
        margin: '48px auto',
        padding: '32px',
        textAlign: 'center',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        gap: '16px',
      }}
    >
      <FolderSearch size={48} color="var(--accent-blue)" aria-hidden="true" />
      <h3 style={{ fontSize: '1.25rem' }}>{title}</h3>
      <p style={{ color: 'var(--text-muted)', lineHeight: 1.6 }}>
        Bu bölüm, SQLite veritabanına kaydedilmiş bir tarama oturumuna bağlıdır.
        Ana ekrandan bir sürücü seçip taramayı tamamlayın veya duraklatılmış oturumdan
        sonuçları görüntüleyin; ardından arama, zaman çizelgesi ve rapor kullanılabilir olur.
      </p>
      {onGoDashboard && (
        <button type="button" className="btn-primary" onClick={onGoDashboard}>
          Ana ekrana git
        </button>
      )}
    </div>
  )
}
