import React, { useEffect, useMemo, useState } from 'react'
import type { FilePreviewResult, FileRecord } from '../../../shared/ipc-contract'
import { formatPreviewHex, previewDataUrl, resolvePreviewImageMime } from '../../../shared/preview-utils'
import { extractJpegExifUnix, extractPdfInfo, sniffMediaContainer } from '../../../shared/embedded-metadata'
import { formatFsTimestamp, getExtension } from './results-view-utils'
import { localizeNote, t, tFormat, localeTag } from '../../i18n'
import InlineAlert from '../InlineAlert'

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
  if (exif != null) return `EXIF · ${new Date(exif * 1000).toLocaleString(localeTag())}`
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
  const previewImgUrl = preview && !imgFailed && preview.kind === 'image' ? previewDataUrl(preview) : null
  const audioUrl = preview && preview.kind === 'audio' ? previewDataUrl(preview) : null
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

  // Escape closes the panel; listener sits above the early return so hooks stay ordered.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [onClose])

  if (!preview && !previewLoading) return null

  return (
    <div className="glass-panel preview-panel">
      <div className="preview-head">
        <strong>{previewRecord ? tFormat('preview.titleWithName', { name: previewRecord.name }) : t('preview.title')}</strong>
        <button type="button" className="btn-secondary btn-compact" onClick={onClose} autoFocus>
          {t('preview.close')}
        </button>
      </div>
      {previewLoading && !preview ? (
        <p className="preview-meta">{t('preview.reading')}</p>
      ) : preview?.success ? (
        <>
          <p className="preview-meta">
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
              className="preview-img"
            />
          ) : preview.kind === 'audio' && audioUrl ? (
            <audio className="preview-audio" controls src={audioUrl} data-testid="preview-audio" />
          ) : preview.kind === 'image' && (imgFailed || !detectedMime) ? (
            <InlineAlert variant="error">{t('preview.imageMismatch')}</InlineAlert>
          ) : preview.kind === 'text' && preview.data ? (
            <pre className="preview-text">
              {new TextDecoder('utf-8', { fatal: false }).decode(preview.data.slice(0, 4096))}
            </pre>
          ) : preview.kind === 'pdf' ? (
            <div className="preview-pdf">
              <p>{t('preview.pdfSummary')}</p>
              <ul>
                {pdfInfo?.version ? <li>{tFormat('preview.pdfVersion', { v: pdfInfo.version })}</li> : null}
                {pdfInfo?.creationDate ? <li>{tFormat('preview.pdfCreated', { d: pdfInfo.creationDate })}</li> : null}
                {pdfInfo?.title ? <li>{tFormat('preview.pdfTitle', { title: pdfInfo.title })}</li> : null}
                {!pdfInfo?.version && !pdfInfo?.creationDate && !pdfInfo?.title ? (
                  <li>{t('preview.pdfNone')}</li>
                ) : null}
              </ul>
            </div>
          ) : preview.kind === 'binary' && (preview.note || mediaHint) ? (
            <div className="preview-binary">
              {preview.note ? (
                <p className={mediaHint && !preview.note.includes('video.ffmpeg.first_frame') ? 'preview-note-gap' : undefined}>
                  {preview.note.startsWith('video.ffmpeg.first_frame') ? (
                    <span className="preview-ffmpeg">{localizeNote(preview.note)}</span>
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
            <p className="preview-meta">{t('preview.noBinary')}</p>
          ) : null}
          {preview.data && preview.data.length > 0 && preview.kind !== 'text' ? (
            <details className="preview-details">
              <summary>
                {t('preview.detailsHex')}
              </summary>
              <pre className="preview-hex">
                {formatPreviewHex(preview.data)}
              </pre>
            </details>
          ) : null}
        </>
      ) : (
        <InlineAlert variant="error">{preview?.error ?? t('preview.failed')}</InlineAlert>
      )}
    </div>
  )
}
