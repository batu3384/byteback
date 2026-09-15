import React, { useState, useEffect } from 'react';
import './ReportGenerator.css';
import { FileText, Download, CheckCircle, Clock, ShieldCheck, PieChart } from 'lucide-react';
import type { ScanSummary } from '../../../shared/ipc-contract';
import { APP_VERSION } from '../../../shared/app-version';
import { htmlEscape } from '../../../shared/html-escape';
import { canGenerateReport } from '../../../shared/scan-required';
import {
  shouldEmitAuditReportSection,
  auditReportLinesKind,
  auditReportChainKind,
  type AuditChainStatus,
} from '../../../shared/report-audit';
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
  const [chainFailed, setChainFailed] = useState(false);

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
      if (alive) {
        setChainFailed(false)
        setChain(v)
      }
    }).catch(() => {
      if (alive) {
        setChain(null)
        setChainFailed(true)
      }
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
      let auditLinesUnread = false;
      let chainStatus: AuditChainStatus | null = null;
      let chainUnread = false;
      try {
        auditLines = (await window.api?.getAuditLog?.(50)) ?? [];
      } catch {
        auditLinesUnread = true;
      }
      try {
        chainStatus = (await window.api?.verifyAuditLog?.()) ?? null;
      } catch {
        chainUnread = true;
      }
      const auditProbe = {
        linesUnread: auditLinesUnread,
        lines: auditLines,
        chainUnread,
        chain: chainStatus,
      };
      const chainKind = auditReportChainKind(auditProbe);
      const chainLine =
        chainKind === 'unread'
          ? `<div><strong>${t('report.chainLabel')}</strong> ${htmlEscape(t('report.chainReadFailed'))}</div>`
          : chainKind === 'ok' && chainStatus
            ? `<div><strong>${t('report.chainLabel')}</strong> ${htmlEscape(tFormat('report.chainOk', { n: String(chainStatus.entries) }))}</div>`
          : chainKind === 'broken' && chainStatus
            ? `<div><strong>${t('report.chainLabel')}</strong> ${htmlEscape(tFormat('report.chainBroken', { line: String(chainStatus.brokenAt), detail: chainStatus.detail }))}</div>`
            : '';
      const linesKind = auditReportLinesKind(auditProbe);
      const auditBody =
        linesKind === 'unread'
          ? `<p>${htmlEscape(t('report.auditUnread'))}</p>`
          : linesKind === 'present'
            ? `<p>${tFormat('report.auditIntro', { n: String(auditLines.length) })}</p>
<table>
  <tr><th>${t('report.auditEventTh')}</th></tr>
  ${auditLines.map((l) => `<tr><td style="font-family:monospace;font-size:0.8rem;">${htmlEscape(l)}</td></tr>`).join('\n  ')}
</table>`
            : '';
      const auditSection = shouldEmitAuditReportSection(auditProbe) ? `
<h2>${t('report.auditHeading')}</h2>
${auditBody}
${chainLine}
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
      let caseUnread = false
      try {
        const caseInfo = await window.api?.getCaseInfo?.()
        caseNumber = caseInfo?.caseNumber?.trim() ?? ''
        investigator = caseInfo?.investigator?.trim() ?? ''
        agency = caseInfo?.agency?.trim() ?? ''
      } catch {
        caseUnread = true
      }

      const body = `
<h1>${t('report.h1')}</h1>
<div class="header-meta">
  <div><strong>${t('report.dateLabel')}</strong> ${htmlEscape(dateStr)} ${htmlEscape(timeStr)}</div>
  <div><strong>${t('report.softwareLabel')}</strong> Byteback v${htmlEscape(APP_VERSION)}</div>
  <div><strong>${t('report.investigatorLabel')}</strong> ${htmlEscape(caseUnread ? t('report.caseUnread') : (investigator || t('report.noInvestigator')))}</div>
</div>

<h2>${t('report.caseInfoHeading')}</h2>
<table>
  <tr><th>${t('report.fieldTh')}</th><th>${t('report.valueTh')}</th></tr>
  <tr><td>${t('report.caseNumberTd')}</td><td>${htmlEscape(caseUnread ? t('report.caseUnread') : (caseNumber || t('report.noCaseNumber')))}</td></tr>
  <tr><td>${t('report.agencyTd')}</td><td>${htmlEscape(caseUnread ? t('report.caseUnread') : (agency || '—'))}</td></tr>
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
    <div className="report-view">
      <div className="report-header glass-panel">
        <div className="examiner-icon ok">
          <FileText size={32} color="var(--success-green)" />
        </div>
        <div>
          <h2>{t('report.title')}</h2>
          <p className="subtitle">{t('report.subtitle')}</p>
        </div>
      </div>

      <div className="report-content glass-panel">

        {!reportAllowed && (
          <InlineAlert variant="warning" title={t('report.scanRequiredTitle')}>
            {t('report.scanRequiredBody')}
          </InlineAlert>
        )}
        {reportAllowed && !reportHtml && !generating && (
          <div className="examiner-empty" role="status" data-testid="report-empty">
            <p>{t('report.emptyBody')}</p>
          </div>
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

        <div className="report-cards">
          <div className="report-card">
            <ShieldCheck size={24} color="var(--accent-blue)" />
            <div>
              <h4>{t('report.custodyCardTitle')}</h4>
              <p>{t('report.custodyCardSub')}</p>
              <p className="card-body">{t('report.custodyCardBody')}</p>
            </div>
          </div>

          <div className="report-card">
            <PieChart size={24} color="var(--accent-blue)" />
            <div>
              <h4>{t('report.foundFilesTitle')}</h4>
              <p>{summary?.totalFiles ?? (scanId > 0 ? '…' : '0')}</p>
            </div>
          </div>

          <div className="report-card" data-testid="report-chain-status" role="status">
            <Clock size={24} color="var(--accent-blue)" />
            <div>
              <h4>{t('report.verifyTitle')}</h4>
              <p>
                {chainFailed
                  ? t('report.chainReadFailed')
                  : chain == null
                  ? t('report.chainChecking')
                  : chain.ok
                    ? tFormat('report.chainOk', { n: String(chain.entries) })
                    : tFormat('report.chainBroken', { line: String(chain.brokenAt), detail: chain.detail })}
              </p>
            </div>
          </div>
        </div>

        <div className="report-included">
          <h3>
            <FileText size={18} color="var(--success-green)" /> {t('report.includedTitle')}
          </h3>
          <ul>
            <li>{t('report.included1')}</li>
            <li>{t('report.included2')}</li>
            <li>{t('report.included3')}</li>
            <li>{t('report.included4')}</li>
          </ul>
        </div>

        <div className="report-actions">
          {!reportHtml && !generating && (
            <button
              type="button"
              className="btn-primary report-generate"
              onClick={generateReport}
              disabled={!reportAllowed}
              title={!reportAllowed ? t('report.generateBlockedTitle') : undefined}
            >
              {t('report.generateBtn')}
            </button>
          )}

          {generating && (
            <div className="generating-state">
              <Clock size={24} className="spinner" />
              <span>{t('report.generating')}</span>
            </div>
          )}

          {reportHtml && !generating && (
            <div className="report-ready">
              <span className="report-ready-title">
                <CheckCircle size={24} /> {t('report.ready')}
              </span>
              <div className="report-hash">
                SHA-256: {reportHash}
              </div>
              {pdfDone && (
                <div className="report-pdf-path">
                  {tFormat('report.pdfSaved', { path: pdfDone })}
                </div>
              )}
              <div className="report-ready-row">
                <button type="button" className="btn-primary" onClick={downloadReport}>
                  <Download size={18} /> {t('report.downloadHtml')}
                </button>
                <button
                  type="button"
                  className="btn-primary report-pdf-btn"
                  onClick={downloadPdf}
                  disabled={pdfBusy}
                >
                  <Download size={18} /> {pdfBusy ? t('report.pdfBuilding') : t('report.downloadPdf')}
                </button>
                <button type="button" className="btn-secondary" onClick={() => { setReportHtml(null); setReportHash(''); setPdfDone(''); }}>
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
