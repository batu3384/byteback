import React, { useEffect, useState } from 'react'
import './CaseView.css'
import { Briefcase, FolderOpen } from 'lucide-react'
import type { CaseInfo, NsrlStats } from '../../../shared/ipc-contract'
import { useI18n, tFormat } from '../../i18n'

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

  const reload = async () => {
    try {
      if (window.api?.getCaseInfo) setInfo(await window.api.getCaseInfo())
      if (window.api?.getNsrlStats) setNsrl(await window.api.getNsrlStats())
    } catch (err) {
      console.error(err)
      setNsrlError(t('case.loadFailed'))
    }
  }

  useEffect(() => {
    void reload()
  }, [])

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
          onChange={(e) => setInfo({ ...info, caseNumber: e.target.value })}
        />

        <label htmlFor="case-investigator">{t('case.investigatorLabel')}</label>
        <input
          id="case-investigator"
          value={info.investigator}
          onChange={(e) => setInfo({ ...info, investigator: e.target.value })}
        />

        <label htmlFor="case-agency">{t('case.agencyLabel')}</label>
        <input
          id="case-agency"
          value={info.agency}
          onChange={(e) => setInfo({ ...info, agency: e.target.value })}
        />

        <label htmlFor="case-notes">{t('case.notesLabel')}</label>
        <textarea
          id="case-notes"
          rows={4}
          value={info.notes}
          onChange={(e) => setInfo({ ...info, notes: e.target.value })}
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
    </div>
  )
}

export default CaseView
