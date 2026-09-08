import React, { useEffect, useRef, useState } from 'react'
import './CaseView.css'
import { Briefcase, FolderOpen, Copy } from 'lucide-react'
import type { CaseInfo, NsrlStats } from '../../../shared/ipc-contract'
import { useI18n, tFormat } from '../../i18n'

interface DataPaths {
  userData: string
  dbPath: string
  sessionLog: string
  auditLog: string
}

const emptyCase: CaseInfo = {
  caseNumber: '',
  investigator: '',
  agency: '',
  notes: '',
  createdAt: 0,
  updatedAt: 0,
}

function CaseView(): React.ReactElement {
  const { t } = useI18n()
  const [info, setInfo] = useState<CaseInfo>(emptyCase)
  const [nsrl, setNsrl] = useState<NsrlStats>({ count: 0, path: '' })
  const [saveError, setSaveError] = useState('')
  const [saved, setSaved] = useState(false)
  const [nsrlError, setNsrlError] = useState('')
  const [dataPaths, setDataPaths] = useState<DataPaths | null>(null)
  const [copiedPath, setCopiedPath] = useState('')
  // Stale-response guard for the mount load (and every unmount, once).
  // Setup re-arms the flag so StrictMode's double mount stays live.
  const aliveRef = useRef(true)
  useEffect(() => {
    aliveRef.current = true
    return () => { aliveRef.current = false }
  }, [])

  const reload = async () => {
    try {
      if (window.api?.getCaseInfo) {
        const loaded = await window.api.getCaseInfo()
        if (aliveRef.current) setInfo(loaded)
      }
      if (window.api?.getNsrlStats) {
        const stats = await window.api.getNsrlStats()
        if (aliveRef.current) setNsrl(stats)
      }
    } catch (err) {
      console.error(err)
      if (aliveRef.current) setNsrlError(t('case.loadFailed'))
    }
  }

  useEffect(() => {
    void reload()
    // Field-evidence paths (field-test protocol 0.2): read-only display data.
    if (window.api?.getDataPaths) {
      window.api.getDataPaths().then((p) => {
        if (aliveRef.current) setDataPaths(p)
      }).catch(() => { /* paths are optional context, not an error surface */ })
    }
  }, [])

  const copyPath = async (label: string, value: string) => {
    try {
      await navigator.clipboard.writeText(value)
      setCopiedPath(label)
    } catch { /* clipboard denied — no-op, paths remain visible/selectable */ }
  }

  const handleSave = async () => {
    setSaveError('')
    setSaved(false)
    if (!window.api?.setCaseInfo) {
      setSaveError(t('case.engineNotReady'))
      return
    }
    try {
      const ok = await window.api.setCaseInfo({
        caseNumber: info.caseNumber,
        investigator: info.investigator,
        agency: info.agency,
        notes: info.notes,
      })
      if (!ok) {
        setSaveError(t('case.saveFailed'))
        return
      }
      setSaved(true)
      await reload()
    } catch (err) {
      console.error(err)
      setSaveError(t('case.saveFailed'))
    }
  }

  const handleNsrl = async () => {
    setNsrlError('')
    if (!window.api?.pickAndLoadNsrl) {
      setNsrlError(t('case.engineNotReady'))
      return
    }
    let result
    try {
      result = await window.api.pickAndLoadNsrl()
    } catch (err) {
      console.error(err)
      setNsrlError(t('case.nsrlLoadFailed'))
      return
    }
    if (!result) return
    if (!result.ok) {
      setNsrlError(t('case.nsrlLoadFailed'))
      return
    }
    setNsrl(result)
  }

  return (
    <div className="case-view">
      <div className="case-header glass-panel">
        <Briefcase size={28} color="var(--accent-blue)" aria-hidden="true" />
        <div>
          <h2>{t('case.title')}</h2>
          <p>{t('case.subtitle')}</p>
        </div>
      </div>

      {saveError && (
        <div className="case-alert" role="alert" tabIndex={-1}>
          <h3>{t('case.saveErrorTitle')}</h3>
          <p>{saveError}</p>
        </div>
      )}

      <form
        className="case-form glass-panel"
        onSubmit={(e) => {
          e.preventDefault()
          void handleSave()
        }}
      >
        <label htmlFor="case-number">{t('case.caseNumberLabel')}</label>
        <input
          id="case-number"
          value={info.caseNumber}
          onChange={(e) => { setInfo({ ...info, caseNumber: e.target.value }); setSaved(false) }}
        />

        <label htmlFor="case-investigator">{t('case.investigatorLabel')}</label>
        <input
          id="case-investigator"
          value={info.investigator}
          onChange={(e) => { setInfo({ ...info, investigator: e.target.value }); setSaved(false) }}
        />

        <label htmlFor="case-agency">{t('case.agencyLabel')}</label>
        <input
          id="case-agency"
          value={info.agency}
          onChange={(e) => { setInfo({ ...info, agency: e.target.value }); setSaved(false) }}
        />

        <label htmlFor="case-notes">{t('case.notesLabel')}</label>
        <textarea
          id="case-notes"
          rows={4}
          value={info.notes}
          onChange={(e) => { setInfo({ ...info, notes: e.target.value }); setSaved(false) }}
        />

        <div className="case-actions">
          <button type="submit" className="btn-primary">{t('case.save')}</button>
          {saved && <span className="case-saved" role="status">{t('case.saved')}</span>}
        </div>
      </form>

      <div className="case-nsrl glass-panel">
        <h3>{t('case.nsrlTitle')}</h3>
        <p>{t('case.nsrlHint')}</p>
        {nsrlError && <p className="case-field-error" role="alert">{nsrlError}</p>}
        <p>{tFormat('case.loadedHashes', { n: String(nsrl.count) })}{nsrl.path ? ` · ${nsrl.path}` : ''}</p>
        {nsrl.count === 0 && nsrl.path ? (
          <p className="case-field-error" role="status">{t('case.zeroLoaded')}</p>
        ) : null}
        <button type="button" className="btn-secondary" onClick={() => void handleNsrl()}>
          <FolderOpen size={16} aria-hidden="true" /> {t('case.pickNsrl')}
        </button>
      </div>

      {dataPaths && (
        <div className="case-nsrl glass-panel" data-testid="evidence-paths">
          <h3>{t('case.pathsTitle')}</h3>
          <p>{t('case.pathsHint')}</p>
          {([
            ['userData', dataPaths.userData],
            ['db', dataPaths.dbPath],
            ['sessionLog', dataPaths.sessionLog],
            ['auditLog', dataPaths.auditLog],
          ] as const).map(([label, value]) => (
            <div key={label} style={{ display: 'flex', alignItems: 'center', gap: '8px', margin: '6px 0' }}>
              <code style={{ flex: 1, fontSize: '0.78rem', wordBreak: 'break-all', color: 'var(--text-muted)' }}>{value}</code>
              <button
                type="button"
                className="btn-secondary"
                aria-label={tFormat('case.copyPath', { label: t(`case.paths.${label}`) })}
                onClick={() => void copyPath(label, value)}
              >
                <Copy size={14} aria-hidden="true" /> {copiedPath === label ? t('case.copied') : t('case.copy')}
              </button>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

export default CaseView
