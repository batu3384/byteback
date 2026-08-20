import React from 'react'
import type { FilePreviewResult, FileRecord } from '../../../shared/ipc-contract'
import { formatPreviewHex, previewDataUrl } from '../../../shared/preview-utils'

interface ResultsPreviewPanelProps {
  preview: FilePreviewResult | null
  previewLoading: boolean
  previewRecord: FileRecord | null | undefined
  onClose: () => void
}

export default function ResultsPreviewPanel({
  preview,
  previewLoading,
  previewRecord,
  onClose,
}: ResultsPreviewPanelProps): React.ReactElement | null {
  if (!preview && !previewLoading) return null

  const previewImgUrl = preview ? previewDataUrl(preview) : null

  return (
    <div className="glass-panel" style={{ padding: '16px 24px', borderLeft: '4px solid var(--accent-blue)' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '12px' }}>
        <strong>Önizleme{previewRecord ? `: ${previewRecord.name}` : ''}</strong>
        <button type="button" className="btn-secondary" style={{ padding: '4px 10px' }} onClick={onClose}>
          Kapat
        </button>
      </div>
      {previewLoading && !preview ? (
        <p style={{ color: 'var(--text-muted)' }}>Okunuyor...</p>
      ) : preview?.success ? (
        <>
          <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem', marginBottom: '8px' }}>
            Tür: {preview.kind ?? 'binary'} · {preview.data?.length ?? 0} bayt
          </p>
          {previewImgUrl ? (
            <img
              src={previewImgUrl}
              alt={previewRecord?.name ?? 'önizleme'}
              style={{ maxWidth: '100%', maxHeight: '240px', objectFit: 'contain', borderRadius: '4px' }}
            />
          ) : preview.kind === 'text' && preview.data ? (
            <pre style={{ fontFamily: 'monospace', fontSize: '0.8rem', whiteSpace: 'pre-wrap', maxHeight: '200px', overflow: 'auto' }}>
              {new TextDecoder('utf-8', { fatal: false }).decode(preview.data.slice(0, 4096))}
            </pre>
          ) : preview.kind === 'pdf' ? (
            <p style={{ color: 'var(--text-muted)' }}>PDF — yapısal önizleme yok; hex dökümü aşağıda.</p>
          ) : null}
          {preview.data && preview.data.length > 0 && preview.kind !== 'text' ? (
            <pre style={{ fontFamily: 'monospace', fontSize: '0.75rem', marginTop: '12px', maxHeight: '160px', overflow: 'auto' }}>
              {formatPreviewHex(preview.data)}
            </pre>
          ) : null}
        </>
      ) : (
        <p style={{ color: 'var(--alert-red)' }}>{preview?.error ?? 'Önizleme alınamadı.'}</p>
      )}
    </div>
  )
}
