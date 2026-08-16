import React, { useEffect, useState, useCallback } from 'react'
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

interface TimelineViewProps {
  scanId: number
}

const PAGE_SIZE = 200

const EVENT_META: Record<string, { label: string; color: string; icon: React.ReactNode }> = {
  create: { label: 'Oluşturma', color: 'var(--success-green)', icon: <FilePlus size={14} /> },
  delete: { label: 'Silme', color: 'var(--alert-red)', icon: <FileMinus size={14} /> },
  rename_old: { label: 'Yeniden Adlandırma (eski)', color: 'var(--warning-yellow)', icon: <FileOutput size={14} /> },
  rename_new: { label: 'Yeniden Adlandırma (yeni)', color: 'var(--warning-yellow)', icon: <FileInput size={14} /> },
  overwrite: { label: 'Üzerine Yazma', color: 'var(--accent-blue)', icon: <FileOutput size={14} /> },
  extend: { label: 'Büyütme', color: 'var(--accent-blue)', icon: <FileInput size={14} /> },
  truncate: { label: 'Kırpma', color: 'var(--warning-yellow)', icon: <FileMinus size={14} /> },
  touch: { label: 'Erişim', color: 'var(--text-muted)', icon: <Activity size={14} /> },
}

function formatTimestamp(unix: number): string {
  if (!unix) return '—'
  const d = new Date(unix * 1000)
  const p = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
}

function TimelineView({ scanId }: TimelineViewProps): React.ReactElement {
  const [events, setEvents] = useState<TimelineEvent[]>([])
  const [total, setTotal] = useState(0)
  const [page, setPage] = useState(0)
  const [filter, setFilter] = useState('')
  const [loading, setLoading] = useState(false)

  const fetchTimeline = useCallback(async (p: number, f: string) => {
    if (scanId < 0) return
    setLoading(true)
    try {
      if (window.api?.getTimelineEvents) {
        const res = await window.api.getTimelineEvents(scanId, p * PAGE_SIZE, PAGE_SIZE, f)
        setEvents(res?.events ?? [])
        setTotal(res?.total ?? 0)
      }
    } catch (err) {
      console.error(err)
    } finally {
      setLoading(false)
    }
  }, [scanId])

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
    <div className="timeline-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%' }}>
      <div className="timeline-header glass-panel" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '24px' }}>
        <div style={{ display: 'flex', gap: '16px', alignItems: 'center' }}>
          <div style={{ background: 'rgba(59, 130, 246, 0.1)', padding: '16px', borderRadius: '12px' }}>
            <Clock size={32} color="var(--accent-blue)" />
          </div>
          <div>
            <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Birleşik Olay Zaman Çizelgesi</h2>
            <p style={{ color: 'var(--text-muted)' }}>
              USN Journal kayıtlarından yeniden kurulmuş dosya olayları — tarama #{scanId}, {total.toLocaleString('tr-TR')} olay
            </p>
          </div>
        </div>
        <button className="btn-secondary" onClick={() => fetchTimeline(page, filter)} disabled={loading} style={{ display: 'flex', gap: '8px' }}>
          <RefreshCw size={16} className={loading ? 'spinner' : ''} /> Yenile
        </button>
      </div>

      <div className="timeline-filters glass-panel" style={{ display: 'flex', gap: '8px', padding: '16px 24px', flexWrap: 'wrap', alignItems: 'center' }}>
        <button
          className={`btn-secondary ${filter === '' ? 'active' : ''}`}
          style={{ padding: '6px 16px', background: filter === '' ? 'var(--panel-border)' : 'transparent' }}
          onClick={() => setFilter('')}
        >
          Tümü
        </button>
        {Object.entries(EVENT_META).map(([key, meta]) => (
          <button
            key={key}
            className={`btn-secondary ${filter === key ? 'active' : ''}`}
            style={{ padding: '6px 16px', background: filter === key ? 'var(--panel-border)' : 'transparent', color: meta.color, display: 'flex', gap: '6px', alignItems: 'center' }}
            onClick={() => setFilter(key)}
          >
            {meta.icon} {meta.label}
          </button>
        ))}
      </div>

      <div className="timeline-content glass-panel" style={{ flex: 1, overflowY: 'auto', padding: '8px 0' }}>
        {loading ? (
          <div style={{ padding: '60px', textAlign: 'center' }}>
            <RefreshCw size={32} className="spinner" style={{ margin: '0 auto 16px', color: 'var(--accent-blue)' }} />
            <p style={{ color: 'var(--text-muted)' }}>Zaman çizelgesi yükleniyor...</p>
          </div>
        ) : events.length === 0 ? (
          <div style={{ padding: '60px', textAlign: 'center' }}>
            <Clock size={48} style={{ margin: '0 auto 16px', color: 'var(--panel-border)' }} />
            <h3 style={{ fontSize: '1.2rem', marginBottom: '8px' }}>Olay Bulunamadı</h3>
            <p style={{ color: 'var(--text-muted)' }}>
              Bu taramada USN Journal olayı yakalanmadı. Journal, NTFS birimlerinde silinen dosyaların ikincil kanıtıdır;
              birim Journal kapalıysa veya kayıt silinmişse olay görüntülenemez.
            </p>
          </div>
        ) : (
          <>
            <div style={{ padding: '8px 24px', fontSize: '0.8rem', color: 'var(--text-muted)' }}>
              Bu sayfada: {Object.entries(typeCounts).map(([t, c]) => `${EVENT_META[t]?.label ?? t}: ${c}`).join(' · ')}
            </div>
            {events.map((ev) => {
              const meta = EVENT_META[ev.eventType] ?? EVENT_META.touch
              return (
                <div
                  key={ev.id}
                  className="timeline-row"
                  style={{
                    display: 'flex',
                    alignItems: 'center',
                    gap: '16px',
                    padding: '10px 24px',
                    borderBottom: '1px solid rgba(255,255,255,0.03)',
                  }}
                >
                  <div
                    className="timeline-marker"
                    style={{
                      width: '10px',
                      height: '10px',
                      borderRadius: '50%',
                      background: meta.color,
                      flexShrink: 0,
                      boxShadow: `0 0 8px ${meta.color}`,
                    }}
                  />
                  <div style={{ fontFamily: 'monospace', fontSize: '0.85rem', color: 'var(--text-muted)', minWidth: '170px', flexShrink: 0 }}>
                    {formatTimestamp(ev.timestamp)}
                  </div>
                  <div style={{ display: 'flex', gap: '6px', alignItems: 'center', color: meta.color, fontSize: '0.8rem', minWidth: '180px', flexShrink: 0 }}>
                    {meta.icon} {meta.label}
                  </div>
                  <div style={{ fontFamily: 'monospace', fontSize: '0.9rem', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} title={ev.fileName}>
                    {ev.fileName || '(isimsiz)'}
                  </div>
                </div>
              )
            })}
          </>
        )}
      </div>

      <div className="timeline-pagination" style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', gap: '16px', paddingBottom: '8px' }}>
        <button className="btn-secondary" onClick={() => goPage(page - 1)} disabled={page === 0 || loading} style={{ display: 'flex', gap: '6px' }}>
          <ChevronLeft size={16} /> Önceki
        </button>
        <span style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>
          Sayfa {page + 1} / {pageCount}
        </span>
        <button className="btn-secondary" onClick={() => goPage(page + 1)} disabled={page + 1 >= pageCount || loading} style={{ display: 'flex', gap: '6px' }}>
          Sonraki <ChevronRight size={16} />
        </button>
      </div>
    </div>
  )
}

export default TimelineView
