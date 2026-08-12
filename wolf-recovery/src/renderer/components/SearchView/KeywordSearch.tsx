import React, { useState } from 'react';
import './KeywordSearch.css';

interface SearchResult {
  fileName: string;
  path: string;
  lineNumber: number;
  context: string;
}

interface KeywordSearchProps {
  filesFound: any[];
}

const KeywordSearch: React.FC<KeywordSearchProps> = ({ filesFound }) => {
  const [query, setQuery] = useState('');
  const [searching, setSearching] = useState(false);
  const [results, setResults] = useState<any[]>([]);
  const [searchDone, setSearchDone] = useState(false);

  const handleSearch = () => {
    if (!query.trim()) return;
    setSearching(true);
    setSearchDone(false);
    
    // In production, this would call native API to search recovered files
    setTimeout(() => {
      setSearching(false);
      setSearchDone(true);
      
      const lowerQuery = query.toLowerCase();
      const filtered = filesFound.filter(f => f.name && f.name.toLowerCase().includes(lowerQuery));
      setResults(filtered);
    }, 500);
  };

  return (
    <div className="keyword-search-view">
      <div className="search-header">
        <h2>Keyword Search</h2>
        <p className="subtitle">Search through recovered files by filename or content keywords.</p>
      </div>

      <div className="search-bar-container glass-panel">
        <div className="search-input-wrapper">
          <span className="search-icon">🔍</span>
          <input
            type="text"
            className="search-input"
            placeholder="Enter keyword (e.g., 'invoice', 'contract', '.xlsx')"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            onKeyDown={(e) => e.key === 'Enter' && handleSearch()}
          />
          <button className="btn-primary search-btn" onClick={handleSearch} disabled={searching}>
            {searching ? 'Searching...' : 'Search'}
          </button>
        </div>

        <div className="search-filters">
          <label className="filter-chip active">
            <input type="checkbox" defaultChecked /> All Files
          </label>
          <label className="filter-chip">
            <input type="checkbox" /> Filename Only
          </label>
          <label className="filter-chip">
            <input type="checkbox" /> Content Search
          </label>
          <label className="filter-chip">
            <input type="checkbox" /> Regex
          </label>
        </div>
      </div>

      <div className="search-results glass-panel">
        {searching && (
          <div className="loading-state">
            <div className="spinner"></div>
            <p>Searching recovered files...</p>
          </div>
        )}

        {searchDone && results.length === 0 && (
          <div className="empty-state">
            <span style={{ fontSize: '3rem' }}>🔎</span>
            <p>No results found for "<strong>{query}</strong>"</p>
            <p className="hint">Try different keywords or check the search filters.</p>
          </div>
        )}

        {results.length > 0 && (
          <div className="results-list">
            {results.map((r, i) => (
              <div key={i} className="result-item">
                <div className="result-file">{r.fileName}</div>
                <div className="result-path">{r.path}</div>
                <div className="result-context">{r.context}</div>
              </div>
            ))}
          </div>
        )}

        {!searching && !searchDone && (
          <div className="empty-state">
            <span style={{ fontSize: '3rem' }}>⌨️</span>
            <p>Enter a keyword to search through recovered files.</p>
          </div>
        )}
      </div>
    </div>
  );
};

export default KeywordSearch;
