import React, { useMemo, useState } from 'react'
import type { FilePreviewResult, FileRecord } from '../../../shared/ipc-contract'
import { formatPreviewHex, previewDataUrl, resolvePreviewImageMime } from '../../../shared/preview-utils'
import { extractJpegExifUnix, extractPdfInfo, sniffMediaContainer } from '../../../shared/embedded-metadata'
import { formatFsTimestamp, getExtension } from './results-view-utils'
import { localizeNote, t, tFormat } from '../../i18n'

interface ResultsPreviewPanelProps {
  preview: FilePreviewResult | null
  previewLoading: boolean
  previewRecord: FileRecord | null | undefined
  onClose: () => void
}

function typeMetaLine(preview: FilePreviewResult, record: FileRecord | null | undefined): string {
  const source = record?.source ?? '—'
  const detected = preview.mime || preview.kind || t('preview.unknown')
  const nameExt = record ? getExtension(record.name) : ''
  const extPart = nameExt ? `.${nameExt}` : t('preview.noExt')
  const mimeLeaf = preview.mime?.includes('/') ? preview.mime.split('/')[1] : ''
  const mismatch =
    Boolean(mimeLeaf) &&
    Boolean(nameExt) &&
    mimeLeaf !== nameExt &&
    !(mimeLeaf === 'jpeg' && (nameExt === 'jpg' || nameExt === 'jpeg'))
  const note = mismatch ? t('preview.extMismatch') : ''
  return tFormat('preview.typeMeta', { source, detected, ext: extPart }) + note
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
        <strong>{previewRecord ? tFormat('preview.titleWithName', { name: previewRecord.name }) : t('preview.title')}</strong>
        <button type="button" className="btn-secondary" style={{ padding: '4px 10px' }} onClick={onClose}>
          {t('preview.close')}
        </button>
      </div>
      {previewLoading && !preview ? (
        <p style={{ color: 'var(--text-muted)' }}>{t('preview.reading')}</p>
      ) : preview?.success ? (
        <>
          <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem', marginBottom: '8px' }}>
            {typeMetaLine(preview, previewRecord)} · {tFormat('preview.bytes', { n: String(preview.data?.length ?? 0) })}
            {(preview.data?.length ?? 0) >= 64 * 1024 ? t('preview.first64kb') : ''}
            {embeddedDate ? ` · ${embeddedDate}` : ''}
            {preview.note ? ` · ${localizeNote(preview.note)}` : ''}
          </p>
          {previewImgUrl ? (
            <img
              key={previewKey}
              src={previewImgUrl}
              alt={previewRecord?.name ?? t('preview.alt')}
              onError={() => setFailedKey(previewKey)}
              style={{ maxWidth: '100%', maxHeight: '240px', objectFit: 'contain', borderRadius: '4px' }}
            />
          ) : preview.kind === 'image' && (imgFailed || !detectedMime) ? (
            <p role="alert" style={{ color: 'var(--alert-red)' }}>
              {t('preview.imageMismatch')}
            </p>
          ) : preview.kind === 'text' && preview.data ? (
            <pre style={{ fontFamily: 'monospace', fontSize: '0.8rem', whiteSpace: 'pre-wrap', maxHeight: '200px', overflow: 'auto' }}>
              {new TextDecoder('utf-8', { fatal: false }).decode(preview.data.slice(0, 4096))}
            </pre>
          ) : preview.kind === 'pdf' ? (
            <div style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>
              <p>{t('preview.pdfSummary')}</p>
              <ul style={{ margin: '4px 0 0 16px' }}>
                {pdfInfo?.version ? <li>{tFormat('preview.pdfVersion', { v: pdfInfo.version })}</li> : null}
                {pdfInfo?.creationDate ? <li>{tFormat('preview.pdfCreated', { d: pdfInfo.creationDate })}</li> : null}
                {pdfInfo?.title ? <li>{tFormat('preview.pdfTitle', { title: pdfInfo.title })}</li> : null}
                {!pdfInfo?.version && !pdfInfo?.creationDate && !pdfInfo?.title ? (
                  <li>{t('preview.pdfNone')}</li>
                ) : null}
              </ul>
            </div>
          ) : preview.kind === 'binary' && (preview.note || mediaHint) ? (
            <div style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>
              {preview.note ? (
                <p style={{ margin: mediaHint && !preview.note.includes('video.ffmpeg.first_frame') ? '0 0 8px' : 0 }}>
                  {preview.note.startsWith('video.ffmpeg.first_frame') ? (
                    <span style={{ color: 'var(--accent-blue)', fontWeight: 600 }}>{localizeNote(preview.note)}</span>
                  ) : (
                    localizeNote(preview.note)
                  )}
                </p>
              ) : null}
              {mediaHint && !preview.note ? (
                <p>
                  {tFormat('preview.mediaContainer', { kind: mediaHint.kind === 'video' ? t('preview.video') : t('preview.audio'), label: mediaHint.label })}
                </p>
              ) : null}
            </div>
          ) : preview.kind === 'binary' ? (
            <p style={{ color: 'var(--text-muted)' }}>{t('preview.noBinary')}</p>
          ) : null}
          {preview.data && preview.data.length > 0 && preview.kind !== 'text' ? (
            <details style={{ marginTop: '12px' }}>
              <summary style={{ cursor: 'pointer', color: 'var(--text-muted)', fontSize: '0.85rem' }}>
                {t('preview.detailsHex')}
              </summary>
              <pre style={{ fontFamily: 'monospace', fontSize: '0.75rem', marginTop: '8px', maxHeight: '160px', overflow: 'auto' }}>
                {formatPreviewHex(preview.data)}
              </pre>
            </details>
          ) : null}
        </>
      ) : (
        <p role="alert" style={{ color: 'var(--alert-red)' }}>{preview?.error ?? t('preview.failed')}</p>
      )}
    </div>
  )
}
