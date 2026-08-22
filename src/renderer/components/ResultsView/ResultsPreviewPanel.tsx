import React, { useMemo, useState } from 'react'
import type { FilePreviewResult, FileRecord } from '../../../shared/ipc-contract'
import { formatPreviewHex, previewDataUrl, resolvePreviewImageMime } from '../../../shared/preview-utils'
import { extractJpegExifUnix, extractPdfInfo, sniffMediaContainer } from '../../../shared/embedded-metadata'
import { formatFsTimestamp, getExtension } from './results-view-utils'

interface ResultsPreviewPanelProps {
  preview: FilePreviewResult | null
  previewLoading: boolean
  previewRecord: FileRecord | null | undefined
  onClose: () => void
}

function typeMetaLine(preview: FilePreviewResult, record: FileRecord | null | undefined): string {
  const source = record?.source ?? '—'
  const detected = preview.mime || preview.kind || 'bilinmiyor'
  const nameExt = record ? getExtension(record.name) : ''
  const extPart = nameExt ? `.${nameExt}` : 'yok'
  const mimeLeaf = preview.mime?.includes('/') ? preview.mime.split('/')[1] : ''
  const mismatch =
    Boolean(mimeLeaf) &&
    Boolean(nameExt) &&
    mimeLeaf !== nameExt &&
    !(mimeLeaf === 'jpeg' && (nameExt === 'jpg' || nameExt === 'jpeg'))
  const note = mismatch ? ' · uzantı yanıltıcı olabilir' : ''
  return `${source} · tespit: ${detected} · ad uzantısı: ${extPart}${note}`
}

function embeddedDateLine(preview: FilePreviewResult, record: FileRecord | null | undefined): string | null {
  if (record?.source?.startsWith('carver') && record.modifiedAt && record.modifiedAt > 0) {
    return formatFsTimestamp(record.modifiedAt, record.source)
  }
  if (!preview.data?.length) return null
  const exif = extractJpegExifUnix(preview.data)
  if (exif != null) return `EXIF · ${new Date(exif * 1000).toLocaleString('tr-TR')}`
  return null
}

export default function ResultsPreviewPanel({
  preview,
  previewLoading,
  previewRecord,
  onClose,
}: ResultsPreviewPanelProps): React.ReactElement | null {
  const previewKey = preview?.data
    ? `${preview.kind}:${preview.data.length}:${preview.mime ?? ''}:${previewRecord?.id ?? ''}`
    : 'none'
  const [failedKey, setFailedKey] = useState<string | null>(null)
  const imgFailed = failedKey === previewKey
  const previewImgUrl = preview && !imgFailed ? previewDataUrl(preview) : null
  const detectedMime = preview ? resolvePreviewImageMime(preview) : null

  const pdfInfo = useMemo(
    () => (preview?.kind === 'pdf' && preview.data ? extractPdfInfo(preview.data) : null),
    [preview?.kind, preview?.data],
  )
  const mediaHint = useMemo(
    () => (preview?.data && preview.kind === 'binary' ? sniffMediaContainer(preview.data) : null),
    [preview?.data, preview?.kind],
  )
  const embeddedDate = preview ? embeddedDateLine(preview, previewRecord) : null

  if (!preview && !previewLoading) return null

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
            {typeMetaLine(preview, previewRecord)} · {preview.data?.length ?? 0} bayt
            {(preview.data?.length ?? 0) >= 64 * 1024 ? ' · ilk 64 KB' : ''}
            {embeddedDate ? ` · ${embeddedDate}` : ''}
            {preview.note ? ` · ${preview.note}` : ''}
          </p>
          {previewImgUrl ? (
            <img
              key={previewKey}
              src={previewImgUrl}
              alt={previewRecord?.name ?? 'önizleme'}
              onError={() => setFailedKey(previewKey)}
              style={{ maxWidth: '100%', maxHeight: '240px', objectFit: 'contain', borderRadius: '4px' }}
            />
          ) : preview.kind === 'image' && (imgFailed || !detectedMime) ? (
            <p role="alert" style={{ color: 'var(--alert-red)' }}>
              Önizleme gösterilemiyor (içerik/tür uyuşmuyor)
            </p>
          ) : preview.kind === 'text' && preview.data ? (
            <pre style={{ fontFamily: 'monospace', fontSize: '0.8rem', whiteSpace: 'pre-wrap', maxHeight: '200px', overflow: 'auto' }}>
              {new TextDecoder('utf-8', { fatal: false }).decode(preview.data.slice(0, 4096))}
            </pre>
          ) : preview.kind === 'pdf' ? (
            <div style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>
              <p>PDF yapısal özet (ilk KB):</p>
              <ul style={{ margin: '4px 0 0 16px' }}>
                {pdfInfo?.version ? <li>Sürüm: {pdfInfo.version}</li> : null}
                {pdfInfo?.creationDate ? <li>Oluşturma (gömülü): {pdfInfo.creationDate}</li> : null}
                {pdfInfo?.title ? <li>Başlık: {pdfInfo.title}</li> : null}
                {!pdfInfo?.version && !pdfInfo?.creationDate && !pdfInfo?.title ? (
                  <li>Gömülü metadata bulunamadı — tam görüntü yok.</li>
                ) : null}
              </ul>
            </div>
          ) : preview.kind === 'binary' && (preview.note || mediaHint) ? (
            <div style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>
              {preview.note ? (
                <p style={{ margin: mediaHint && !preview.note.includes('FFmpeg') ? '0 0 8px' : 0 }}>
                  {preview.note.includes('FFmpeg ilk kare') ? (
                    <span style={{ color: 'var(--accent-blue)', fontWeight: 600 }}>{preview.note}</span>
                  ) : (
                    preview.note
                  )}
                </p>
              ) : null}
              {mediaHint && !preview.note ? (
                <p>
                  {mediaHint.kind === 'video' ? 'Video' : 'Ses'} konteyneri: {mediaHint.label}. Tam oynatma önizlemesi yok — kurtarmadan önce başka araçla doğrula.
                </p>
              ) : null}
            </div>
          ) : preview.kind === 'binary' ? (
            <p style={{ color: 'var(--text-muted)' }}>Görsel/metin önizlemesi yok (binary).</p>
          ) : null}
          {preview.data && preview.data.length > 0 && preview.kind !== 'text' ? (
            <details style={{ marginTop: '12px' }}>
              <summary style={{ cursor: 'pointer', color: 'var(--text-muted)', fontSize: '0.85rem' }}>
                Detaylar (hex)
              </summary>
              <pre style={{ fontFamily: 'monospace', fontSize: '0.75rem', marginTop: '8px', maxHeight: '160px', overflow: 'auto' }}>
                {formatPreviewHex(preview.data)}
              </pre>
            </details>
          ) : null}
        </>
      ) : (
        <p role="alert" style={{ color: 'var(--alert-red)' }}>{preview?.error ?? 'Önizleme alınamadı.'}</p>
      )}
    </div>
  )
}
