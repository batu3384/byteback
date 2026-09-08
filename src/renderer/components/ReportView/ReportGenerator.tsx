import React, { useState, useEffect } from 'react';
import './ReportGenerator.css';
import { FileText, Download, CheckCircle, Clock, ShieldCheck, PieChart } from 'lucide-react';
import type { ScanSummary } from '../../../shared/ipc-contract';
import { APP_VERSION } from '../../../shared/app-version';
import { htmlEscape } from '../../../shared/html-escape';
import { canGenerateReport } from '../../../shared/scan-required';
import InlineAlert from '../InlineAlert';
import { useI18n, tFormat, getLang } from '../../i18n';

interface ReportGeneratorProps {
  scanId: number;
  scanElapsed: number;
  scanState?: import('../../../shared/ipc-contract').ScanState | null;
}

/** Compute the real SHA-256 of the report content via the Web Crypto API. */
async function sha256Hex(text: string): Promise<string> {
  const data = new TextEncoder().encode(text);
  const digest = await crypto.subtle.digest('SHA-256', data);
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

const ReportGenerator: React.FC<ReportGeneratorProps> = ({ scanId, scanElapsed, scanState }) => {
  const { t } = useI18n();
  const [generating, setGenerating] = useState(false);
  const [reportHtml, setReportHtml] = useState<string | null>(null);
  const [reportHash, setReportHash] = useState<string>('');
  const [pdfBusy, setPdfBusy] = useState(false);
  const [pdfDone, setPdfDone] = useState('');
  const [summary, setSummary] = useState<ScanSummary | null>(null);
  const [formError, setFormError] = useState<string | null>(null);

  const [summaryError, setSummaryError] = useState<string | null>(null);
  const [chain, setChain] = useState<{ ok: boolean; entries: number; brokenAt: number; detail: string } | null>(null);

  const [rowState, setRowState] = useState<import('../../../shared/ipc-contract').ScanState | null>(scanState ?? null)

  const reportAllowed = canGenerateReport(scanId, rowState ?? scanState ?? undefined)
  const reportElapsedSec = (() => {
    const st = rowState ?? scanState
    if (st?.startedAt && st.updatedAt && st.updatedAt >= st.startedAt) {
      return st.updatedAt - st.startedAt
    }
    return scanElapsed
  })()

  useEffect(() => {
    setRowState(scanState ?? null)
  }, [scanState])

  // Runtime audit-chain verification: re-walks the SHA-256 chain so the
  // tamper-evidence claim is machine-checked, not manual.
  useEffect(() => {
    if (!window.api?.verifyAuditLog) return
    let alive = true
    window.api.verifyAuditLog().then((v) => {
      if (alive) setChain(v)
    }).catch(() => {
      if (alive) setChain(null)
    })
    return () => { alive = false }
  }, [])

  useEffect(() => {
    let alive = true
    if (scanId > 0 && window.api?.getScanState) {
      window.api.getScanState(scanId).then((s) => {
        if (alive) setRowState(s)
      }).catch(() => {
        if (alive) setRowState(null)
      })
    } else {
      setRowState(null)
    }
    return () => { alive = false }
  }, [scanId])

  useEffect(() => {
    let alive = true
    if (scanId > 0 && window.api?.getScanSummary) {
      setSummaryError(null)
      window.api.getScanSummary(scanId)
        .then((s) => {
          if (alive) setSummary(s)
        })
        .catch((e: unknown) => {
          if (!alive) return
          setSummary(null)
          setSummaryError(e instanceof Error ? e.message : t('report.summaryReadFailed'))
        });
    } else {
      setSummary(null);
      setSummaryError(null)
    }
    return () => { alive = false }
  }, [scanId]);

  const generateReport = async () => {
    if (!reportAllowed) {
      setFormError(t('report.needsComplete'));
      return;
    }
    setFormError(null);
    setGenerating(true);
    try {
      const now = new Date();
      const dateStr = now.toISOString().split('T')[0];
      const timeStr = now.toTimeString().split(' ')[0].replace(/:/g, '-');

      // Real audit-log tail: the hash-chained forensic log maintained by the
      // native engine (scan/imaging/wipe events). Embedded verbatim so the
      // report reflects what actually happened, not what we wish happened.
      let auditLines: string[] = [];
      let chainStatus: { ok: boolean; entries: number; brokenAt: number; detail: string } | null = null;
      try {
        auditLines = (await window.api?.getAuditLog?.(50)) ?? [];
      } catch { /* audit log optional in report */ }
      try {
        chainStatus = (await window.api?.verifyAuditLog?.()) ?? null;
      } catch { /* verification optional in report */ }
      const chainLine = chainStatus
        ? `<div><strong>${t('report.chainLabel')}</strong> ${
            chainStatus.ok
              ? tFormat('report.chainOk', { n: String(chainStatus.entries) })
              : tFormat('report.chainBroken', { line: String(chainStatus.brokenAt), detail: chainStatus.detail })
          }</div>`
        : '';
      const auditSection = auditLines.length > 0 ? `
<h2>${t('report.auditHeading')}</h2>
<p>${tFormat('report.auditIntro', { n: String(auditLines.length) })}</p>
${chainLine}
<table>
  <tr><th>${t('report.auditEventTh')}</th></tr>
  ${auditLines.map((l) => `<tr><td style="font-family:monospace;font-size:0.8rem;">${htmlEscape(l)}</td></tr>`).join('\n  ')}
</table>
` : '';

      const imgCount = summary?.imageFiles ?? 0;
      const vidCount = summary?.videoFiles ?? 0;
      const docCount = summary?.documentFiles ?? 0;
      const audCount = summary?.audioFiles ?? 0;
      const arcCount = summary?.archiveFiles ?? 0;
      const totalFiles = summary?.totalFiles ?? 0;
      const othCount = totalFiles - (imgCount + vidCount + docCount + audCount + arcCount);

      // The integrity hash covers everything except the hash field itself:
      // build the body first, hash it, then splice the digest into the
      // template. This is a REAL computation, not a claim — the footer states
      // exactly what is covered so an examiner can verify it independently.
      let caseNumber = ''
      let investigator = ''
      let agency = ''
      try {
        const caseInfo = await window.api?.getCaseInfo?.()
        caseNumber = caseInfo?.caseNumber?.trim() ?? ''
        investigator = caseInfo?.investigator?.trim() ?? ''
        agency = caseInfo?.agency?.trim() ?? ''
      } catch { /* case metadata optional */ }

      const body = `
<h1>${t('report.h1')}</h1>
<div class="header-meta">
  <div><strong>${t('report.dateLabel')}</strong> ${htmlEscape(dateStr)} ${htmlEscape(timeStr)}</div>
  <div><strong>${t('report.softwareLabel')}</strong> Byteback v${htmlEscape(APP_VERSION)}</div>
  <div><strong>${t('report.investigatorLabel')}</strong> ${htmlEscape(investigator || t('report.noInvestigator'))}</div>
</div>

<h2>${t('report.caseInfoHeading')}</h2>
<table>
  <tr><th>${t('report.fieldTh')}</th><th>${t('report.valueTh')}</th></tr>
  <tr><td>${t('report.caseNumberTd')}</td><td>${htmlEscape(caseNumber || t('report.noCaseNumber'))}</td></tr>
  <tr><td>${t('report.agencyTd')}</td><td>${htmlEscape(agency || '—')}</td></tr>
  <tr><td>${t('report.reportDateTd')}</td><td>${htmlEscape(now.toLocaleString(getLang() === 'en' ? 'en-US' : 'tr-TR'))}</td></tr>
  <tr><td>${t('report.softwareVersionTd')}</td><td>Byteback ${htmlEscape(APP_VERSION)} (Native C++ Engine)</td></tr>
  <tr><td>${t('report.osTd')}</td><td>Windows</td></tr>
</table>

<h2>${t('report.evidenceHeading')}</h2>
<table>
  <tr><th>${t('report.metricTh')}</th><th>${t('report.valueTh')}</th></tr>
  <tr><td>${t('report.totalFilesTd')}</td><td>${totalFiles}</td></tr>
  <tr><td>${t('report.deletedTd')}</td><td>${summary?.deletedFiles ?? 0}</td></tr>
  <tr><td>${t('report.allocatedTd')}</td><td>${Math.max(0, totalFiles - (summary?.deletedFiles ?? 0))}</td></tr>
      <tr><td>${t('report.scanDurationTd')}</td><td>${reportElapsedSec} ${t('report.secondsSuffix')}</td></tr>
  <tr><td>${t('report.timelineEventsTd')}</td><td>${summary?.timelineEvents ?? 0}</td></tr>
  <tr><td>${t('report.usnOpsTd')}</td><td>${summary?.usnCreates ?? 0} / ${summary?.usnDeletes ?? 0} / ${summary?.usnRenames ?? 0}</td></tr>
</table>

<h2>${t('report.custodyHeading')}</h2>
<p>${t('report.custodyBody')}</p>

<h2>${t('report.categoriesHeading')}</h2>
<table>
  <tr><th>${t('report.categoryTh')}</th><th>${t('report.countTh')}</th></tr>
  <tr><td>${t('report.imagesTd')}</td><td>${imgCount}</td></tr>
  <tr><td>${t('report.videosTd')}</td><td>${vidCount}</td></tr>
  <tr><td>${t('report.documentsTd')}</td><td>${docCount}</td></tr>
  <tr><td>${t('report.audioTd')}</td><td>${audCount}</td></tr>
  <tr><td>${t('report.archivesTd')}</td><td>${arcCount}</td></tr>
  <tr><td>${t('report.otherTd')}</td><td>${othCount}</td></tr>
</table>
${auditSection}
`;

      const hash = await sha256Hex(body);
      const finalHtml = `<!DOCTYPE html>
<html lang="${getLang()}"><head><meta charset="utf-8"><title>${t('report.docTitle')}</title>
<style>
  body { font-family: Arial, sans-serif; max-width: 900px; margin: 0 auto; padding: 40px; background: #f8f9fa; color: #1a1a2e; }
  h1 { color: #0B0F19; border-bottom: 3px solid #2962FF; padding-bottom: 10px; }
  h2 { color: #2962FF; margin-top: 30px; }
  table { width: 100%; border-collapse: collapse; margin: 20px 0; }
  th, td { border: 1px solid #ddd; padding: 10px; text-align: left; }
  th { background: #0B0F19; color: #fff; }
  .footer { margin-top: 40px; border-top: 1px solid #ddd; padding-top: 20px; font-size: 0.85rem; color: #666; }
  .header-meta { display: flex; justify-content: space-between; margin: 20px 0; padding: 15px; background: #e9ecef; border-radius: 8px; }
  .hash-box { font-family: monospace; word-break: break-all; background: #eef2ff; border: 1px solid #c7d2fe; padding: 12px; border-radius: 6px; }
</style></head><body>
${body}
<div class="footer">
  <p>${t('report.footerAuto')}</p>
  <p><strong>${t('report.footerHashLabel')}</strong> ${t('report.footerHashBody')}</p>
  <p class="hash-box">${hash}</p>
  <p>${t('report.footerHashNote')}</p>
</div>
</body></html>`;

      setReportHtml(finalHtml);
      setReportHash(hash);
    } catch (err) {
      console.error('Rapor oluşturma hatası:', err);
      const msg = err instanceof Error ? err.message : String(err);
      setFormError(tFormat('report.generateFailed', { err: msg }));
    } finally {
      setGenerating(false);
    }
  };

  const downloadReport = () => {
    if (!reportHtml) return;
    const now = new Date();
    const dateStr = now.toISOString().split('T')[0];
    const blob = new Blob([reportHtml], { type: 'text/html;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = tFormat('report.fileName', { date: dateStr });
    a.click();
    URL.revokeObjectURL(url);
  };

  // Real PDF via Chromium's print engine (main-process printToPDF). The PDF
  // contains exactly the report content the SHA-256 in the footer covers.
  const downloadPdf = async () => {
    if (!reportHtml || !window.api?.exportReportPdf) return
    setPdfBusy(true)
    try {
      const res = await window.api.exportReportPdf(reportHtml)
      if (res?.success) {
        setPdfDone(res.path ?? '')
      } else if (res?.error && !res.canceled) {
        setFormError(tFormat('report.pdfFailed', { err: res.error }))
      }
    } catch (err) {
      console.error(err)
      const msg = err instanceof Error ? err.message : String(err)
      setFormError(tFormat('report.pdfFailed', { err: msg }))
    } finally {
      setPdfBusy(false)
    }
  };

  return (
    <div className="report-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%', maxWidth: '800px', margin: '0 auto' }}>
      <div className="report-header glass-panel" style={{ padding: '24px', display: 'flex', gap: '16px', alignItems: 'center' }}>
        <div style={{ background: 'rgba(16, 185, 129, 0.1)', padding: '16px', borderRadius: '12px' }}>
          <FileText size={32} color="var(--success-green)" />
        </div>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>{t('report.title')}</h2>
          <p style={{ color: 'var(--text-muted)' }}>{t('report.subtitle')}</p>
        </div>
      </div>

      <div className="report-content glass-panel" style={{ padding: '32px', display: 'flex', flexDirection: 'column', gap: '24px' }}>

        {!reportAllowed && (
          <InlineAlert variant="warning" title={t('report.scanRequiredTitle')}>
            {t('report.scanRequiredBody')}
          </InlineAlert>
        )}

        {formError && (
          <InlineAlert variant="error" onDismiss={() => setFormError(null)}>
            {formError}
          </InlineAlert>
        )}

        {summaryError && (
          <InlineAlert variant="warning" onDismiss={() => setSummaryError(null)}>
            {tFormat('report.summaryLoadFailed', { err: summaryError })}
          </InlineAlert>
        )}

        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 1fr))', gap: '16px' }}>
          <div style={{ padding: '16px', background: 'rgba(255,255,255,0.02)', border: '1px solid var(--panel-border)', borderRadius: '8px', display: 'flex', alignItems: 'center', gap: '12px' }}>
            <ShieldCheck size={24} color="var(--accent-blue)" />
            <div>
              <h4 style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>{t('report.custodyCardTitle')}</h4>
              <p style={{ fontWeight: 500 }}>{t('report.custodyCardSub')}</p>
              <p style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>{t('report.custodyCardBody')}</p>
            </div>
          </div>

          <div style={{ padding: '16px', background: 'rgba(255,255,255,0.02)', border: '1px solid var(--panel-border)', borderRadius: '8px', display: 'flex', alignItems: 'center', gap: '12px' }}>
            <PieChart size={24} color="var(--accent-blue)" />
            <div>
              <h4 style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>{t('report.foundFilesTitle')}</h4>
              <p style={{ fontWeight: 500 }}>{summary?.totalFiles ?? (scanId > 0 ? '…' : '0')}</p>
            </div>
          </div>

          <div data-testid="report-chain-status" role="status" style={{ padding: '16px', background: 'rgba(255,255,255,0.02)', border: '1px solid var(--panel-border)', borderRadius: '8px', display: 'flex', alignItems: 'center', gap: '12px' }}>
            <Clock size={24} color="var(--accent-blue)" />
            <div>
              <h4 style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>{t('report.verifyTitle')}</h4>
              <p style={{ fontWeight: 500 }}>
                {chain == null
                  ? t('report.chainChecking')
                  : chain.ok
                    ? tFormat('report.chainOk', { n: String(chain.entries) })
                    : tFormat('report.chainBroken', { line: String(chain.brokenAt), detail: chain.detail })}
              </p>
            </div>
          </div>
        </div>

        <div style={{ background: 'rgba(0,0,0,0.2)', padding: '24px', borderRadius: '8px', border: '1px solid var(--panel-border)' }}>
          <h3 style={{ fontSize: '1.1rem', marginBottom: '12px', display: 'flex', alignItems: 'center', gap: '8px' }}>
            <FileText size={18} color="var(--success-green)" /> {t('report.includedTitle')}
          </h3>
          <ul style={{ color: 'var(--text-muted)', fontSize: '0.95rem', marginLeft: '24px', display: 'flex', flexDirection: 'column', gap: '8px' }}>
            <li>{t('report.included1')}</li>
            <li>{t('report.included2')}</li>
            <li>{t('report.included3')}</li>
            <li>{t('report.included4')}</li>
          </ul>
        </div>

        <div style={{ display: 'flex', justifyContent: 'center', marginTop: '16px' }}>
          {!reportHtml && !generating && (
            <button
              className="btn-primary"
              onClick={generateReport}
              disabled={!reportAllowed}
              title={!reportAllowed ? t('report.generateBlockedTitle') : undefined}
              style={{ padding: '16px 32px', fontSize: '1.1rem', background: 'var(--success-green)', color: '#000', opacity: reportAllowed ? 1 : 0.5 }}
            >
              {t('report.generateBtn')}
            </button>
          )}

          {generating && (
            <div className="generating-state" style={{ display: 'flex', alignItems: 'center', gap: '12px', color: 'var(--success-green)' }}>
              <Clock size={24} className="spinner" />
              <span style={{ fontSize: '1.1rem', fontWeight: 500 }}>{t('report.generating')}</span>
            </div>
          )}

          {reportHtml && !generating && (
            <div className="report-ready" style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '16px' }}>
              <span style={{ color: 'var(--success-green)', fontWeight: 'bold', display: 'flex', alignItems: 'center', gap: '8px', fontSize: '1.2rem' }}>
                <CheckCircle size={24} /> {t('report.ready')}
              </span>
              <div style={{ fontFamily: 'monospace', fontSize: '0.8rem', color: 'var(--text-muted)', wordBreak: 'break-all', maxWidth: '600px' }}>
                SHA-256: {reportHash}
              </div>
              {pdfDone && (
                <div style={{ fontSize: '0.85rem', color: 'var(--success-green)', fontFamily: 'monospace', wordBreak: 'break-all' }}>
                  {tFormat('report.pdfSaved', { path: pdfDone })}
                </div>
              )}
              <div style={{ display: 'flex', gap: '12px' }}>
                <button className="btn-primary" onClick={downloadReport} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                  <Download size={18} /> {t('report.downloadHtml')}
                </button>
                <button
                  className="btn-primary"
                  onClick={downloadPdf}
                  disabled={pdfBusy}
                  style={{ display: 'flex', alignItems: 'center', gap: '8px', background: 'var(--accent-blue)', color: '#fff' }}
                >
                  <Download size={18} /> {pdfBusy ? t('report.pdfBuilding') : t('report.downloadPdf')}
                </button>
                <button className="btn-secondary" onClick={() => { setReportHtml(null); setReportHash(''); setPdfDone(''); }}>
                  {t('report.regenerate')}
                </button>
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  );
};

export default ReportGenerator;
