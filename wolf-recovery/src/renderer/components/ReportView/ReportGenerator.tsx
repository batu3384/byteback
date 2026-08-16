import React, { useState } from 'react';
import './ReportGenerator.css';
import { FileText, Download, CheckCircle, Clock, ShieldCheck, PieChart } from 'lucide-react';

interface ReportGeneratorProps {
  scanResults?: any;
  filesFound?: any[];
}

/** Compute the real SHA-256 of the report content via the Web Crypto API. */
async function sha256Hex(text: string): Promise<string> {
  const data = new TextEncoder().encode(text);
  const digest = await crypto.subtle.digest('SHA-256', data);
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

const ReportGenerator: React.FC<ReportGeneratorProps> = ({ scanResults, filesFound = [] }) => {
  const [generating, setGenerating] = useState(false);
  const [reportHtml, setReportHtml] = useState<string | null>(null);
  const [reportHash, setReportHash] = useState<string>('');

  const generateReport = async () => {
    setGenerating(true);
    try {
      const now = new Date();
      const dateStr = now.toISOString().split('T')[0];
      const timeStr = now.toTimeString().split(' ')[0].replace(/:/g, '-');

      const imgCount = filesFound.filter((f: any) => f.category === 'Image').length;
      const vidCount = filesFound.filter((f: any) => f.category === 'Video').length;
      const docCount = filesFound.filter((f: any) => f.category === 'Document').length;
      const audCount = filesFound.filter((f: any) => f.category === 'Audio').length;
      const arcCount = filesFound.filter((f: any) => f.category === 'Archive').length;
      const othCount = filesFound.length - (imgCount + vidCount + docCount + audCount + arcCount);

      // The integrity hash covers everything except the hash field itself:
      // build the body first, hash it, then splice the digest into the
      // template. This is a REAL computation, not a claim — the footer states
      // exactly what is covered so an examiner can verify it independently.
      const body = `
<h1>🐺 Wolf Recovery — Adli Bilişim (Forensic) Kurtarma Raporu</h1>
<div class="header-meta">
  <div><strong>Tarih:</strong> ${dateStr} ${timeStr}</div>
  <div><strong>Yazılım:</strong> Wolf Recovery</div>
  <div><strong>Uzman:</strong> [İsim Girin]</div>
</div>

<h2>1. Vaka Bilgileri</h2>
<table>
  <tr><th>Alan</th><th>Değer</th></tr>
  <tr><td>Vaka Numarası</td><td>WR-${Date.now()}</td></tr>
  <tr><td>Rapor Tarihi</td><td>${now.toLocaleString('tr-TR')}</td></tr>
  <tr><td>Yazılım Sürümü</td><td>Wolf Recovery (Native C++ Engine)</td></tr>
  <tr><td>İşletim Sistemi</td><td>Windows</td></tr>
</table>

<h2>2. Delil (Kanıt) Özeti</h2>
<table>
  <tr><th>Metrik</th><th>Değer</th></tr>
  <tr><td>Bulunan Toplam Dosya</td><td>${scanResults?.totalFiles || filesFound.length}</td></tr>
  <tr><td>Kurtarılabilir (Tam)</td><td>${scanResults?.recoverableFiles ?? filesFound.filter((f: any) => f.status === 0).length}</td></tr>
  <tr><td>Kısmen Üzerine Yazılmış</td><td>${scanResults?.partialFiles ?? filesFound.filter((f: any) => f.status !== 0).length}</td></tr>
  <tr><td>Tarama Süresi</td><td>${scanResults?.duration ?? 'Kayıt yok'}</td></tr>
</table>

<h2>3. Gözetim Zinciri (Chain of Custody)</h2>
<p>Bu tarama sırasındaki tüm disk erişim işlemleri Salt-Okunur (Read-Only) modda gerçekleştirilmiştir.
Motor, fiziksel diske yalnızca okuma amacıyla açmakta (GENERIC_READ) ve imajlama dışında hiçbir
yazma işlemi yapmamaktadır. Kaynak medyanın içerik bütünlüğü tarama boyunca korunmuştur.</p>

<h2>4. Dosya Kategorileri Dağılımı</h2>
<table>
  <tr><th>Kategori</th><th>Dosya Sayısı</th></tr>
  <tr><td>📸 Resimler</td><td>${imgCount}</td></tr>
  <tr><td>🎬 Videolar</td><td>${vidCount}</td></tr>
  <tr><td>📄 Belgeler</td><td>${docCount}</td></tr>
  <tr><td>🎵 Ses Dosyaları</td><td>${audCount}</td></tr>
  <tr><td>📦 Arşivler</td><td>${arcCount}</td></tr>
  <tr><td>💾 Diğer</td><td>${othCount}</td></tr>
</table>
`;

      const hash = await sha256Hex(body);
      const finalHtml = `<!DOCTYPE html>
<html lang="tr"><head><meta charset="utf-8"><title>Wolf Recovery - Adli Bilişim Raporu</title>
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
  <p>Bu rapor Wolf Recovery tarafından otomatik olarak oluşturulmuştur.
  Raporu mahkemeye veya kuruma sunmadan önce doğruluk kontrolü uzmanın sorumluluğundadır.</p>
  <p><strong>Rapor Bütünlük Doğrulaması (SHA-256):</strong> Aşağıdaki özet, bu raporun
  1-4 numaralı bölümlerinin tam içeriği üzerinden hesaplanmıştır:</p>
  <p class="hash-box">${hash}</p>
  <p>Rapor içeriğinde yapılan herhangi bir değişiklik bu özeti geçersiz kılar.
  Kurtarılan bireysel dosyaların MD5 özetleri, kurtarma işlemi sonunda
  kurtarma sonuçlarıyla birlikte ayrıca raporlanır.</p>
</div>
</body></html>`;

      setReportHtml(finalHtml);
      setReportHash(hash);
    } catch (err) {
      console.error('Rapor oluşturma hatası:', err);
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
    a.download = `wolf-recovery-rapor-${dateStr}.html`;
    a.click();
    URL.revokeObjectURL(url);
  };

  return (
    <div className="report-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%', maxWidth: '800px', margin: '0 auto' }}>
      <div className="report-header glass-panel" style={{ padding: '24px', display: 'flex', gap: '16px', alignItems: 'center' }}>
        <div style={{ background: 'rgba(16, 185, 129, 0.1)', padding: '16px', borderRadius: '12px' }}>
          <FileText size={32} color="var(--success-green)" />
        </div>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Rapor Oluşturucu (Forensic Report)</h2>
          <p style={{ color: 'var(--text-muted)' }}>Kurtarma operasyonunuz için Adli Bilişim (Forensic) standartlarında resmi rapor oluşturun.</p>
        </div>
      </div>

      <div className="report-content glass-panel" style={{ padding: '32px', display: 'flex', flexDirection: 'column', gap: '24px' }}>

        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 1fr))', gap: '16px' }}>
          <div style={{ padding: '16px', background: 'rgba(255,255,255,0.02)', border: '1px solid var(--panel-border)', borderRadius: '8px', display: 'flex', alignItems: 'center', gap: '12px' }}>
            <ShieldCheck size={24} color="var(--accent-blue)" />
            <div>
              <h4 style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Gözetim Zinciri</h4>
              <p style={{ fontWeight: 500 }}>Salt-Okunur Erişim</p>
            </div>
          </div>

          <div style={{ padding: '16px', background: 'rgba(255,255,255,0.02)', border: '1px solid var(--panel-border)', borderRadius: '8px', display: 'flex', alignItems: 'center', gap: '12px' }}>
            <PieChart size={24} color="var(--accent-blue)" />
            <div>
              <h4 style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Bulunan Dosyalar</h4>
              <p style={{ fontWeight: 500 }}>{filesFound.length}</p>
            </div>
          </div>

          <div style={{ padding: '16px', background: 'rgba(255,255,255,0.02)', border: '1px solid var(--panel-border)', borderRadius: '8px', display: 'flex', alignItems: 'center', gap: '12px' }}>
            <Clock size={24} color="var(--accent-blue)" />
            <div>
              <h4 style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Rapor Doğrulaması</h4>
              <p style={{ fontWeight: 500 }}>SHA-256 (Gerçek Hesap)</p>
            </div>
          </div>
        </div>

        <div style={{ background: 'rgba(0,0,0,0.2)', padding: '24px', borderRadius: '8px', border: '1px solid var(--panel-border)' }}>
          <h3 style={{ fontSize: '1.1rem', marginBottom: '12px', display: 'flex', alignItems: 'center', gap: '8px' }}>
            <FileText size={18} color="var(--success-green)" /> Rapora Dahil Edilecekler
          </h3>
          <ul style={{ color: 'var(--text-muted)', fontSize: '0.95rem', marginLeft: '24px', display: 'flex', flexDirection: 'column', gap: '8px' }}>
            <li>Vaka bilgileri ve tarama istatistikleri</li>
            <li>Adli gözetim zinciri beyanı (Chain of Custody)</li>
            <li>Dosya kategorilerine göre dağılım analizi</li>
            <li>Rapor içeriğinin SHA-256 bütünlük özeti (raporla birlikte hesaplanır ve gömülür)</li>
          </ul>
        </div>

        <div style={{ display: 'flex', justifyContent: 'center', marginTop: '16px' }}>
          {!reportHtml && !generating && (
            <button className="btn-primary" onClick={generateReport} style={{ padding: '16px 32px', fontSize: '1.1rem', background: 'var(--success-green)', color: '#000' }}>
              RESMİ RAPOR OLUŞTUR
            </button>
          )}

          {generating && (
            <div className="generating-state" style={{ display: 'flex', alignItems: 'center', gap: '12px', color: 'var(--success-green)' }}>
              <Clock size={24} className="spinner" />
              <span style={{ fontSize: '1.1rem', fontWeight: 500 }}>Adli Bilişim raporu hazırlanıyor...</span>
            </div>
          )}

          {reportHtml && !generating && (
            <div className="report-ready" style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '16px' }}>
              <span style={{ color: 'var(--success-green)', fontWeight: 'bold', display: 'flex', alignItems: 'center', gap: '8px', fontSize: '1.2rem' }}>
                <CheckCircle size={24} /> Rapor Hazır
              </span>
              <div style={{ fontFamily: 'monospace', fontSize: '0.8rem', color: 'var(--text-muted)', wordBreak: 'break-all', maxWidth: '600px' }}>
                SHA-256: {reportHash}
              </div>
              <div style={{ display: 'flex', gap: '12px' }}>
                <button className="btn-primary" onClick={downloadReport} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                  <Download size={18} /> HTML Olarak İndir
                </button>
                <button className="btn-secondary" onClick={() => { setReportHtml(null); setReportHash(''); }}>
                  Yeniden Oluştur
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
