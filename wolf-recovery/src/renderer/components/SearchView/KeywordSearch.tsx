import React, { useState } from 'react';
import './KeywordSearch.css';
import { Search, FileText, Filter, AlertCircle, FileSearch, Keyboard } from 'lucide-react';

interface KeywordSearchProps {
  filesFound: any[];
}

const KeywordSearch: React.FC<KeywordSearchProps> = ({ filesFound }) => {
  const [query, setQuery] = useState('');
  const [searching, setSearching] = useState(false);
  const [results, setResults] = useState<any[]>([]);
  const [searchDone, setSearchDone] = useState(false);
  const [useRegex, setUseRegex] = useState(false);
  const [regexError, setRegexError] = useState('');

  const handleSearch = () => {
    if (!query.trim()) return;
    setSearching(true);
    setSearchDone(false);
    setRegexError('');

    // CA-009 fix: the Regex checkbox now actually changes the matching.
    // An invalid pattern is reported instead of silently ignored.
    let filtered: any[] = [];
    if (useRegex) {
      try {
        const re = new RegExp(query, 'i');
        filtered = filesFound.filter(f => f.name && re.test(f.name));
      } catch (err: any) {
        setRegexError(`Geçersiz regex: ${err?.message ?? err}`);
        setSearching(false);
        return;
      }
    } else {
      const lowerQuery = query.toLowerCase();
      filtered = filesFound.filter(f => f.name && f.name.toLowerCase().includes(lowerQuery));
    }
    setResults(filtered);
    setSearching(false);
    setSearchDone(true);
  };

  return (
    <div className="keyword-search-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%' }}>
      <div className="search-header glass-panel" style={{ padding: '24px' }}>
        <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Kelime Araması (Keyword Search)</h2>
        <p style={{ color: 'var(--text-muted)' }}>Kurtarılan dosyaların isminde veya içeriğinde anahtar kelime araması yapın.</p>
      </div>

      <div className="search-bar-container glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '16px' }}>
        <div className="search-input-wrapper" style={{ display: 'flex', gap: '12px' }}>
          <div style={{ flex: 1, display: 'flex', alignItems: 'center', background: 'rgba(0,0,0,0.2)', border: '1px solid var(--panel-border)', borderRadius: '8px', padding: '0 16px' }}>
            <Search size={20} color="var(--text-muted)" />
            <input
              type="text"
              style={{ flex: 1, background: 'transparent', border: 'none', padding: '12px 16px', color: 'var(--text-main)', outline: 'none', fontSize: '1rem' }}
              placeholder="Anahtar kelime girin (örn. 'fatura', 'sözleşme', '.xlsx')"
              value={query}
              maxLength={200}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={(e) => e.key === 'Enter' && handleSearch()}
            />
          </div>
          <button className="btn-primary search-btn" onClick={handleSearch} disabled={searching} style={{ padding: '0 32px' }}>
            {searching ? 'Aranıyor...' : 'Ara'}
          </button>
        </div>

        {regexError && (
          <div style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--alert-red)', fontSize: '0.85rem' }}>
            <AlertCircle size={16} /> {regexError}
          </div>
        )}

        <div className="search-filters" style={{ display: 'flex', gap: '16px', color: 'var(--text-muted)', fontSize: '0.9rem' }}>
          <span style={{ display: 'flex', alignItems: 'center', gap: '8px', color: 'var(--text-main)' }}>
            <Filter size={16} /> Filtreler:
          </span>
          <label style={{ display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer' }}>
            <input type="checkbox" defaultChecked /> Tüm Dosyalar
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer' }}>
            <input type="checkbox" defaultChecked /> Sadece Dosya Adı
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer', opacity: 0.5 }}>
            <input type="checkbox" disabled /> İçerik Araması (Yakında)
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer' }}>
            <input
              type="checkbox"
              checked={useRegex}
              onChange={(e) => { setUseRegex(e.target.checked); setRegexError(''); }}
              id="regex-toggle"
            />
            <label htmlFor="regex-toggle" style={{ cursor: 'pointer' }}> Düzenli İfade (Regex)</label>
          </label>
        </div>
      </div>

      <div className="search-results glass-panel" style={{ flex: 1, overflow: 'hidden', display: 'flex', flexDirection: 'column' }}>
        {searching && (
          <div className="loading-state" style={{ margin: 'auto', textAlign: 'center', color: 'var(--accent-blue)' }}>
            <Search size={48} className="spinner" style={{ margin: '0 auto 16px' }} />
            <p style={{ color: 'var(--text-muted)' }}>Bulunan dosyalar taranıyor...</p>
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
                <div key={i} style={{ 
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
