import React, { useEffect, useState } from 'react'
import './CaseView.css'
import { Briefcase, FolderOpen } from 'lucide-react'
import type { CaseInfo, NsrlStats } from '../../../shared/ipc-contract'

const emptyCase: CaseInfo = {
  caseNumber: '',
  investigator: '',
  agency: '',
  notes: '',
  createdAt: 0,
  updatedAt: 0,
}

function CaseView(): React.ReactElement {
  const [info, setInfo] = useState<CaseInfo>(emptyCase)
  const [nsrl, setNsrl] = useState<NsrlStats>({ count: 0, path: '' })
  const [saveError, setSaveError] = useState('')
  const [saved, setSaved] = useState(false)
  const [nsrlError, setNsrlError] = useState('')

  const reload = async () => {
    if (window.api?.getCaseInfo) setInfo(await window.api.getCaseInfo())
    if (window.api?.getNsrlStats) setNsrl(await window.api.getNsrlStats())
  }

  useEffect(() => {
    void reload()
  }, [])

  const handleSave = async () => {
    setSaveError('')
    setSaved(false)
    if (!window.api?.setCaseInfo) {
      setSaveError('Motor hazır değil.')
      return
    }
    const ok = await window.api.setCaseInfo({
      caseNumber: info.caseNumber,
      investigator: info.investigator,
      agency: info.agency,
      notes: info.notes,
    })
    if (!ok) {
      setSaveError('Dava kaydı yazılamadı.')
      return
    }
    setSaved(true)
    await reload()
  }

  const handleNsrl = async () => {
    setNsrlError('')
    if (!window.api?.pickAndLoadNsrl) {
      setNsrlError('Motor hazır değil.')
      return
    }
    const result = await window.api.pickAndLoadNsrl()
    if (!result) return
    if (!result.ok) {
      setNsrlError('NSRL dosyası yüklenemedi. 32 karakter hex satırları veya CSV ilk sütun beklenir.')
      return
    }
    setNsrl(result)
  }

  return (
    <div className="case-view">
      <div className="case-header glass-panel">
        <Briefcase size={28} color="var(--accent-blue)" aria-hidden="true" />
        <div>
          <h2>Dava ve hash seti</h2>
          <p>E01 başlığı ve adli rapor bu alanları kullanır. NSRL bellek içi MD5 setidir; tam RDS değildir.</p>
        </div>
      </div>

      {saveError && (
        <div className="case-alert" role="alert" tabIndex={-1}>
          <h3>Kayıt başarısız</h3>
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
        <label htmlFor="case-number">Dava numarası</label>
        <input
          id="case-number"
          value={info.caseNumber}
          onChange={(e) => setInfo({ ...info, caseNumber: e.target.value })}
        />

        <label htmlFor="case-investigator">Uzman / investigator</label>
        <input
          id="case-investigator"
          value={info.investigator}
          onChange={(e) => setInfo({ ...info, investigator: e.target.value })}
        />

        <label htmlFor="case-agency">Kurum</label>
        <input
          id="case-agency"
          value={info.agency}
          onChange={(e) => setInfo({ ...info, agency: e.target.value })}
        />

        <label htmlFor="case-notes">Notlar</label>
        <textarea
          id="case-notes"
          rows={4}
          value={info.notes}
          onChange={(e) => setInfo({ ...info, notes: e.target.value })}
        />

        <div className="case-actions">
          <button type="submit" className="btn-primary">Kaydet</button>
          {saved && <span className="case-saved">Kaydedildi. Sonraki E01 bu numarayı taşır.</span>}
        </div>
      </form>

      <div className="case-nsrl glass-panel">
        <h3>NSRL MD5 seti</h3>
        <p>Kullanıcı dosyası: 32 hex MD5 herhangi CSV sütununda. Resmi RDS SHA-1 ilk sütunu atlanır. Gömülü RDS yok.</p>
        {nsrlError && <p className="case-field-error" role="alert">{nsrlError}</p>}
        <p>Yüklü hash: {nsrl.count}{nsrl.path ? ` · ${nsrl.path}` : ''}</p>
        {nsrl.count === 0 && nsrl.path ? (
          <p className="case-field-error" role="status">0 MD5 yüklendi. SHA-1 sütunu atlandı veya dosya boş.</p>
        ) : null}
        <button type="button" className="btn-secondary" onClick={() => void handleNsrl()}>
          <FolderOpen size={16} aria-hidden="true" /> NSRL dosyası seç
        </button>
      </div>
    </div>
  )
}

export default CaseView
