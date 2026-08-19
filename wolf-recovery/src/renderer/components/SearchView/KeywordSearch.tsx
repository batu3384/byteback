import React, { useState, useEffect, useRef } from 'react';
import './KeywordSearch.css';
import { Search, FileText, Filter, AlertCircle, FileSearch, Keyboard } from 'lucide-react';
import type { FileRecord } from '../../../shared/ipc-contract';

interface KeywordSearchProps {
  scanId: number;
}

const CATEGORIES = [
  { value: '', label: 'Tüm Kategoriler' },
  { value: 'Image', label: 'Görsel' },
  { value: 'Document', label: 'Belge' },
  { value: 'Video', label: 'Video' },
  { value: 'Audio', label: 'Ses' },
  { value: 'Archive', label: 'Arşiv' },
];

const KeywordSearch: React.FC<KeywordSearchProps> = ({ scanId }) => {
  const [query, setQuery] = useState('');
  const [searching, setSearching] = useState(false);
  const [results, setResults] = useState<FileRecord[]>([]);
  const [searchDone, setSearchDone] = useState(false);
  const [useRegex, setUseRegex] = useState(false);
  const [searchContent, setSearchContent] = useState(false);
  const [category, setCategory] = useState('');
  const [regexError, setRegexError] = useState('');
  const [progress, setProgress] = useState({ current: 0, total: 0 });
  const cleanupRef = useRef<(() => void)[]>([]);

  useEffect(() => {
    return () => {
      cleanupRef.current.forEach((fn) => fn());
      cleanupRef.current = [];
      window.api?.stopContentSearch?.();
    };
  }, []);

  const handleSearch = async () => {
    if (!query.trim()) return;
    if (scanId <= 0) {
      setRegexError('Önce bir tarama tamamlayın.');
      return;
    }

    cleanupRef.current.forEach((fn) => fn());
    cleanupRef.current = [];
    window.api?.stopContentSearch?.();

    setSearching(true);
    setSearchDone(false);
    setRegexError('');
    setResults([]);
    setProgress({ current: 0, total: 0 });

    if (useRegex && !searchContent) {
      try {
        new RegExp(query, 'i');
      } catch (err: any) {
        setRegexError(`Geçersiz regex: ${err?.message ?? err}`);
        setSearching(false);
        return;
      }
    }

    if (searchContent && window.api.startContentSearch) {
      const matches: FileRecord[] = [];

      if (window.api.onContentSearchProgress) {
        cleanupRef.current.push(
          window.api.onContentSearchProgress((data) => {
            setProgress({ current: data.current, total: data.total });
          }),
        );
      }
      if (window.api.onContentSearchMatch) {
        cleanupRef.current.push(
          window.api.onContentSearchMatch((data) => {
            matches.push(data as FileRecord);
            setResults([...matches]);
          }),
        );
      }
      if (window.api.onContentSearchComplete) {
        cleanupRef.current.push(
          window.api.onContentSearchComplete(() => {
            setSearching(false);
            setSearchDone(true);
            cleanupRef.current.forEach((fn) => fn());
            cleanupRef.current = [];
          }),
        );
      }

      try {
        const ok = await window.api.startContentSearch(scanId, query);
        if (!ok) {
          setSearching(false);
          setSearchDone(true);
        }
      } catch {
        setSearching(false);
        setSearchDone(true);
      }
      return;
    }

    try {
      const filtered = await window.api.searchFiles(
        scanId,
        query,
        0,
        500,
        useRegex,
        category || undefined,
      );
      setResults(filtered);
    } catch {
      setResults([]);
    }
    setSearching(false);
    setSearchDone(true);
  };

  const handleStop = () => {
    window.api?.stopContentSearch?.();
    cleanupRef.current.forEach((fn) => fn());
    cleanupRef.current = [];
    setSearching(false);
    setSearchDone(true);
  };

  const progressPct = progress.total > 0 ? Math.round((progress.current / progress.total) * 100) : 0;

  return (
    <div className="keyword-search-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%' }}>
      <div className="search-header glass-panel" style={{ padding: '24px' }}>
        <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Kelime Araması (Keyword Search)</h2>
        <p style={{ color: 'var(--text-muted)' }}>SQLite metadata araması veya dosya içeriğinde (ilk 256 KB) metin araması.</p>
      </div>

      <div className="search-bar-container glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '16px' }}>
        <div className="search-input-wrapper" style={{ display: 'flex', gap: '12px' }}>
          <div style={{ flex: 1, display: 'flex', alignItems: 'center', background: 'rgba(0,0,0,0.2)', border: '1px solid var(--panel-border)', borderRadius: '8px', padding: '0 16px' }}>
            <Search size={20} color="var(--text-muted)" />
            <input
              type="text"
              style={{ flex: 1, background: 'transparent', border: 'none', padding: '12px 16px', color: 'var(--text-main)', fontSize: '1rem' }}
              placeholder="Anahtar kelime girin (örn. 'fatura', 'sözleşme', '.xlsx')"
              value={query}
              maxLength={200}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={(e) => e.key === 'Enter' && !searching && handleSearch()}
            />
          </div>
          <button className="btn-primary search-btn" onClick={handleSearch} disabled={searching} style={{ padding: '0 32px' }}>
            {searching ? 'Aranıyor...' : 'Ara'}
          </button>
          {searching && searchContent && (
            <button className="btn-secondary" onClick={handleStop} style={{ padding: '0 16px' }}>
              Durdur
            </button>
          )}
        </div>

        {regexError && (
          <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--alert-red)', fontSize: '0.85rem' }}>
            <AlertCircle size={16} /> {regexError}
          </div>
        )}

        <div className="search-filters" style={{ display: 'flex', gap: '16px', color: 'var(--text-muted)', fontSize: '0.9rem', flexWrap: 'wrap', alignItems: 'center' }}>
          <span style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-main)' }}>
            <Filter size={16} /> Filtreler:
          </span>
          <label style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            Kategori:
            <select
              value={category}
              disabled={searchContent}
              onChange={(e) => setCategory(e.target.value)}
              style={{ background: 'rgba(0,0,0,0.2)', color: 'var(--text-main)', border: '1px solid var(--panel-border)', borderRadius: '4px', padding: '4px 8px' }}
            >
              {CATEGORIES.map((c) => (
                <option key={c.value || 'all'} value={c.value}>{c.label}</option>
              ))}
            </select>
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer' }}>
            <input
              type="checkbox"
              checked={searchContent}
              onChange={(e) => { setSearchContent(e.target.checked); if (e.target.checked) setUseRegex(false); }}
              id="content-toggle"
            />
            <label htmlFor="content-toggle" style={{ cursor: 'pointer' }}>İçerik Araması</label>
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: '8px', cursor: useRegex ? 'pointer' : 'not-allowed', opacity: searchContent ? 0.5 : 1 }}>
            <input
              type="checkbox"
              checked={useRegex}
              disabled={searchContent}
              onChange={(e) => { setUseRegex(e.target.checked); setRegexError(''); }}
              id="regex-toggle"
            />
            <label htmlFor="regex-toggle" style={{ cursor: 'pointer' }}>Düzenli İfade (Regex)</label>
          </label>
        </div>
      </div>

      <div className="search-results glass-panel" style={{ flex: 1, overflow: 'hidden', display: 'flex', flexDirection: 'column' }}>
        {searching && (
          <div className="loading-state" style={{ margin: 'auto', textAlign: 'center', color: 'var(--accent-blue)' }}>
            <Search size={48} className="spinner" style={{ margin: '0 auto 16px' }} />
            <p style={{ color: 'var(--text-muted)' }}>
              {searchContent && progress.total > 0
                ? `Dosya içeriği taranıyor… ${progressPct}% (${progress.current}/${progress.total})`
                : 'Bulunan dosyalar taranıyor...'}
            </p>
          </div>
        )}

        {searchDone && results.length === 0 && (
          <div className="empty-state" style={{ margin: 'auto', textAlign: 'center' }}>
            <AlertCircle size={48} color="var(--warning-yellow)" style={{ margin: '0 auto 16px' }} />
            <p style={{ fontSize: '1.1rem', marginBottom: '8px' }}>"<strong>{query}</strong>" için sonuç bulunamadı</p>
            <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>Farklı anahtar kelimeler deneyin veya arama filtrelerini kontrol edin.</p>
          </div>
        )}

        {results.length > 0 && (
          <div className="results-list" style={{ padding: '16px 24px', overflowY: 'auto' }}>
            <p style={{ color: 'var(--text-muted)', marginBottom: '16px' }}>{results.length} sonuç bulundu.</p>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
              {results.map((r, i) => (
                <div key={`${r.id}-${i}`} style={{
                  display: 'flex', alignItems: 'center', padding: '12px 16px',
                  background: 'rgba(255,255,255,0.02)', borderRadius: '6px', border: '1px solid transparent'
                }}>
                  <FileText size={18} style={{ color: 'var(--accent-blue)', marginRight: '12px' }} />
                  <span style={{ fontWeight: 500, flex: 1 }}>{r.name}</span>
                  <span style={{ color: 'var(--text-muted)', width: '200px', fontSize: '0.9rem' }}>{r.path || r.category || 'Kurtarılanlar'}</span>
                  <span style={{ color: 'var(--text-muted)', width: '100px', textAlign: 'right', fontSize: '0.9rem' }}>
                    {r.sizeBytes ? (r.sizeBytes / 1024).toFixed(2) + ' KB' : ''}
                  </span>
                </div>
              ))}
            </div>
          </div>
        )}

        {!searching && !searchDone && (
          <div className="empty-state" style={{ margin: 'auto', textAlign: 'center', color: 'var(--panel-border)' }}>
            <Keyboard size={64} style={{ margin: '0 auto 16px' }} />
            <p style={{ color: 'var(--text-muted)' }}>Kurtarılan dosyalar arasında arama yapmak için bir kelime girin.</p>
          </div>
        )}
      </div>
    </div>
  );
};

export default KeywordSearch;
