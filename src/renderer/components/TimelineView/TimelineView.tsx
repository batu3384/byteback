import React, { useEffect, useState, useCallback, useRef } from 'react'
import './TimelineView.css'
import {
  Clock,
  FilePlus,
  FileMinus,
  FileOutput,
  FileInput,
  RefreshCw,
  ChevronLeft,
  ChevronRight,
  Activity,
} from 'lucide-react'
import type { TimelineEvent } from '../../../shared/types'
import { useI18n, tFormat, formatInt } from '../../i18n'
import InlineAlert from '../InlineAlert'

interface TimelineViewProps {
  scanId: number
}

const PAGE_SIZE = 200

const EVENT_META: Record<string, { labelKey: string; color: string; icon: React.ReactNode }> = {
  create: { labelKey: 'tl.event.create', color: 'var(--success-green)', icon: <FilePlus size={14} /> },
  delete: { labelKey: 'tl.event.delete', color: 'var(--alert-red)', icon: <FileMinus size={14} /> },
  rename_old: { labelKey: 'tl.event.rename_old', color: 'var(--warning-yellow)', icon: <FileOutput size={14} /> },
  rename_new: { labelKey: 'tl.event.rename_new', color: 'var(--warning-yellow)', icon: <FileInput size={14} /> },
  overwrite: { labelKey: 'tl.event.overwrite', color: 'var(--accent-blue)', icon: <FileOutput size={14} /> },
  extend: { labelKey: 'tl.event.extend', color: 'var(--accent-blue)', icon: <FileInput size={14} /> },
  truncate: { labelKey: 'tl.event.truncate', color: 'var(--warning-yellow)', icon: <FileMinus size={14} /> },
  touch: { labelKey: 'tl.event.touch', color: 'var(--text-muted)', icon: <Activity size={14} /> },
}

function formatTimestamp(unix: number): string {
  if (!unix) return '—'
  const d = new Date(unix * 1000)
  const p = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
}

function TimelineView({ scanId }: TimelineViewProps): React.ReactElement {
  const { t } = useI18n()
  const [events, setEvents] = useState<TimelineEvent[]>([])
  const [total, setTotal] = useState(0)
  const [page, setPage] = useState(0)
  const [filter, setFilter] = useState('')
  const [loading, setLoading] = useState(false)
  const [loadError, setLoadError] = useState('')
  // Stale-response guard: navigating away mid-fetch must not setState.
  // Setup re-arms the flag so StrictMode's double mount stays live.
  const aliveRef = useRef(true)
  useEffect(() => {
    aliveRef.current = true
    return () => { aliveRef.current = false }
  }, [])

  const fetchTimeline = useCallback(async (p: number, f: string) => {
    if (scanId < 0) return
    setLoading(true)
    setLoadError('')
    try {
      if (window.api?.getTimelineEvents) {
        const res = await window.api.getTimelineEvents(scanId, p * PAGE_SIZE, PAGE_SIZE, f)
        if (!aliveRef.current) return
        setEvents(res?.events ?? [])
        setTotal(res?.total ?? 0)
      }
    } catch (err) {
      console.error(err)
      if (aliveRef.current) setLoadError(t('tl.loadFailed'))
    } finally {
      if (aliveRef.current) setLoading(false)
    }
  }, [scanId, t])

  useEffect(() => {
    setPage(0)
    fetchTimeline(0, filter)
  }, [fetchTimeline, filter])

  const goPage = (p: number) => {
    setPage(p)
    fetchTimeline(p, filter)
  }

  const pageCount = Math.max(1, Math.ceil(total / PAGE_SIZE))
  const typeCounts = events.reduce<Record<string, number>>((acc, e) => {
    acc[e.eventType] = (acc[e.eventType] ?? 0) + 1
    return acc
  }, {})

  return (
    <div className="timeline-view">
      <div className="timeline-header glass-panel">
        <div className="timeline-header-info">
          <div className="examiner-icon">
            <Clock size={32} color="var(--accent-blue)" />
          </div>
          <div>
            <h2>{t('tl.title')}</h2>
            <p>
              {tFormat('tl.subtitle', { n: String(scanId), total: formatInt(total) })}
            </p>
          </div>
        </div>
        <button className="btn-secondary" data-testid="timeline-refresh" onClick={() => fetchTimeline(page, filter)} disabled={loading}>
          <RefreshCw size={16} className={loading ? 'spinner' : ''} /> {t('dash.refresh')}
        </button>
      </div>

      <div className="timeline-filters glass-panel">
        <button
          type="button"
          className={`btn-secondary ${filter === '' ? 'active' : ''}`}
          aria-pressed={filter === ''}
          data-testid="timeline-filter-all"
          onClick={() => setFilter('')}
        >
          {t('scan.all')}
        </button>
        {Object.entries(EVENT_META).map(([key, meta]) => (
          <button
            key={key}
            type="button"
            className={`btn-secondary ${filter === key ? 'active' : ''}`}
            style={{ '--event-color': meta.color } as React.CSSProperties}
            aria-pressed={filter === key}
            data-testid={`timeline-filter-${key}`}
            onClick={() => setFilter(key)}
          >
            {meta.icon} {t(meta.labelKey)}
          </button>
        ))}
      </div>

      <div className="timeline-content glass-panel">
        {loading ? (
          <div className="examiner-empty timeline-load" role="status" data-testid="timeline-load">
            <RefreshCw size={32} className="spinner" />
            <p>{t('tl.loading')}</p>
          </div>
        ) : loadError ? (
          <InlineAlert variant="error" testId="timeline-error">{loadError}</InlineAlert>
        ) : events.length === 0 ? (
          <div className="examiner-empty" role="status" data-testid="timeline-empty">
            <Clock size={48} className="timeline-empty-ico" aria-hidden="true" />
            <h3>{t('tl.emptyTitle')}</h3>
            <p>
              {t('tl.emptyBody')}
            </p>
          </div>
        ) : (
          <>
            <div className="timeline-summary">
              {tFormat('tl.pageSummary', { list: Object.entries(typeCounts).map(([k, c]) => `${EVENT_META[k] ? t(EVENT_META[k].labelKey) : k}: ${c}`).join(' · ') })}
            </div>
            {events.map((ev) => {
              const meta = EVENT_META[ev.eventType] ?? EVENT_META.touch
              return (
                <div
                  key={ev.id}
                  className="timeline-row"
                  data-testid="timeline-row"
                  style={{ '--event-color': meta.color } as React.CSSProperties}
                >
                  <div className="timeline-marker" />
                  <div className="timeline-time">
                    {formatTimestamp(ev.timestamp)}
                  </div>
                  <div className="timeline-kind">
                    {meta.icon} {t(meta.labelKey)}
                  </div>
                  <div className="timeline-file" title={ev.fileName}>
                    {ev.fileName || t('tl.unnamed')}
                  </div>
                </div>
              )
            })}
          </>
        )}
      </div>

      <div className="timeline-pagination">
        <button type="button" className="btn-secondary" data-testid="timeline-prev" onClick={() => goPage(page - 1)} disabled={page === 0 || loading}>
          <ChevronLeft size={16} aria-hidden="true" /> {t('scan.prev')}
        </button>
        <span className="timeline-page">
          {tFormat('tl.pageOf', { cur: String(page + 1), total: String(pageCount) })}
        </span>
        <button type="button" className="btn-secondary" data-testid="timeline-next" onClick={() => goPage(page + 1)} disabled={page + 1 >= pageCount || loading}>
          {t('scan.next')} <ChevronRight size={16} aria-hidden="true" />
        </button>
      </div>
    </div>
  )
}

export default TimelineView
