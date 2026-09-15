import React, { useState, useEffect, useRef } from 'react';
import './KeywordSearch.css';
import { Search, FileText, Filter, AlertCircle, Keyboard } from 'lucide-react';
import type { FileRecord } from '../../../shared/ipc-contract';
import { isContentSearchOpenFailed, isContentSearchQueryTooLong, isContentSearchRegexRejected, isContentSearchReadIncomplete, CONTENT_SEARCH_MAX_QUERY_BYTES, CONTENT_SEARCH_MAX_REGEX_CHARS, contentQueryByteLength } from '../../../shared/content-search-status';
import { useI18n, tFormat } from '../../i18n';
import { buildMatchParts, buildSnippetParts, isSafeHighlightRegex, type SnippetRecord } from './highlight';
import InlineAlert from '../InlineAlert';

interface KeywordSearchProps {
  scanId: number;
}

/** Wraps the matched query substring in <mark class="kw-mark">. The IPC
 *  payload (FileRecord) carries no content snippet, so highlighting applies
 *  to the name/path text that the results list actually shows. */
function HighlightText({ text, query, useRegex }: { text: string; query: string; useRegex: boolean }): React.ReactElement {
  const parts = buildMatchParts(text, query, useRegex);
  return (
    <>
      {parts.map((p, i) =>
        p.match ? (
          <mark key={i} className="kw-mark">{p.text}</mark>
        ) : (
          <React.Fragment key={i}>{p.text}</React.Fragment>
        ),
      )}
    </>
  );
}

/** Native content-search snippet (FileRecord.snippet): one sanitized context
 *  line with the [matchStart,matchEnd) span marked. Invalid/absent offsets
 *  (native may send -1) degrade to plain, unhighlighted text. */
function SnippetText({ snippet, start, end }: { snippet: string; start?: number; end?: number }): React.ReactElement {
  const parts = buildSnippetParts(snippet, start, end);
  return (
    <>
      {parts.map((p, i) =>
        p.match ? (
          <mark key={i} className="kw-mark">{p.text}</mark>
        ) : (
          <React.Fragment key={i}>{p.text}</React.Fragment>
        ),
      )}
    </>
  );
}

const CATEGORIES = [
  { value: '', labelKey: 'kw.catAll' },
  { value: 'Image', labelKey: 'kw.catImage' },
  { value: 'Document', labelKey: 'scan.document' },
  { value: 'Video', labelKey: 'scan.video' },
  { value: 'Audio', labelKey: 'scan.audio' },
  { value: 'Archive', labelKey: 'scan.archive' },
];

const KeywordSearch: React.FC<KeywordSearchProps> = ({ scanId }) => {
  const { t } = useI18n();
  const [query, setQuery] = useState('');
  const [searching, setSearching] = useState(false);
  const [results, setResults] = useState<FileRecord[]>([]);
  const [searchDone, setSearchDone] = useState(false);
  const [useRegex, setUseRegex] = useState(false);
  const [searchContent, setSearchContent] = useState(false);
  const [category, setCategory] = useState('');
  const [regexError, setRegexError] = useState('');
  const [searchError, setSearchError] = useState('');
  const [contentUnread, setContentUnread] = useState(false);
  const [progress, setProgress] = useState({ current: 0, total: 0 });
  const cleanupRef = useRef<(() => void)[]>([]);
  // Content-search matches accumulate in a ref; the visible list gets one
  // copy per 100ms flush instead of a whole-array copy + full list re-render
  // per match event (the old setResults([...matches]) was O(n²) in copies).
  const matchesRef = useRef<FileRecord[]>([]);
  const flushTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const flushMatches = (): void => {
    setResults(matchesRef.current.slice());
  };
  const stopFlush = (): void => {
    if (flushTimerRef.current != null) {
      clearInterval(flushTimerRef.current);
      flushTimerRef.current = null;
    }
  };

  useEffect(() => {
    return () => {
      stopFlush();
      cleanupRef.current.forEach((fn) => fn());
      cleanupRef.current = [];
      window.api?.stopContentSearch?.();
    };
  }, []);

  const handleSearch = async () => {
    if (!query.trim()) return;
    if (useRegex && query.length > CONTENT_SEARCH_MAX_REGEX_CHARS) {
      setRegexError(t('kw.regexTooLong'));
      return;
    }
    if (searchContent && contentQueryByteLength(query) > CONTENT_SEARCH_MAX_QUERY_BYTES) {
      setRegexError(t('kw.contentQueryTooLong'));
      return;
    }
    if (scanId <= 0) {
      setRegexError(t('kw.needScan'));
      return;
    }

    cleanupRef.current.forEach((fn) => fn());
    cleanupRef.current = [];
    window.api?.stopContentSearch?.();

    setSearching(true);
    setSearchDone(false);
    setRegexError('');
    setSearchError('');
    setContentUnread(false);
    stopFlush();
    matchesRef.current = [];
    setResults([]);
    setProgress({ current: 0, total: 0 });

    if (useRegex) {
      try {
        new RegExp(query, 'i');
      } catch (err: any) {
        setRegexError(tFormat('kw.invalidRegex', { err: err?.message ?? String(err) }));
        setSearching(false);
        return;
      }
      if (!isSafeHighlightRegex(query)) {
        setRegexError(t('kw.regexUnsafe'));
        setSearching(false);
        return;
      }
    }

    if (searchContent && window.api.startContentSearch) {
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
            matchesRef.current.push(data as FileRecord);
          }),
        );
      }
      if (window.api.onContentSearchComplete) {
        cleanupRef.current.push(
          window.api.onContentSearchComplete((data) => {
            stopFlush();
            flushMatches();
            setSearching(false);
            setSearchDone(true);
            if (isContentSearchOpenFailed(data.status)) {
              setSearchError(t('kw.contentBindFailed'));
            } else if (isContentSearchQueryTooLong(data.status)) {
              setSearchError(t('kw.contentQueryTooLong'));
            } else if (isContentSearchRegexRejected(data.status)) {
              setSearchError(t('kw.regexRejected'));
            } else if (isContentSearchReadIncomplete(data.status)) {
              setContentUnread(true);
            }
            cleanupRef.current.forEach((fn) => fn());
            cleanupRef.current = [];
          }),
        );
      }

      // Batched flush while matches stream in: one copy + render per 100ms.
      stopFlush();
      flushTimerRef.current = setInterval(flushMatches, 100);

      try {
        const res = await window.api.startContentSearch(scanId, query, useRegex);
        if (!res.ok) {
          stopFlush();
          flushMatches();
          setRegexError(
            res.error === 'content query too long'
              ? t('kw.contentQueryTooLong')
              : res.error === 'regex rejected'
                ? t('kw.regexRejected')
              : (res.error ?? t('kw.contentStartFailed')),
          )
          setSearching(false);
          setSearchDone(true);
        }
      } catch (e: unknown) {
        stopFlush();
        flushMatches();
        setRegexError(e instanceof Error ? e.message : t('kw.unexpectedError'))
        setSearching(false);
        setSearchDone(true);
      }
      return;
    }

    try {
      const res = await window.api.searchFiles(
        scanId,
        query,
        0,
        500,
        useRegex,
        category || undefined,
      );
      if (res.error) {
        setSearchError(tFormat('kw.searchError', { err: res.error }));
        setResults([]);
      } else {
        setResults(res.rows);
      }
    } catch {
      setSearchError(t('kw.unexpectedError'));
      setResults([]);
    }
    setSearching(false);
    setSearchDone(true);
  };

  const handleStop = () => {
    window.api?.stopContentSearch?.();
    stopFlush();
    flushMatches();
    cleanupRef.current.forEach((fn) => fn());
    cleanupRef.current = [];
    setSearching(false);
    setSearchDone(true);
  };

  const progressPct = progress.total > 0 ? Math.round((progress.current / progress.total) * 100) : 0;

  return (
    <div className="keyword-search-view">
      <div className="search-header glass-panel">
        <div className="examiner-icon">
          <Search size={32} color="var(--accent-blue)" aria-hidden="true" />
        </div>
        <div>
          <h2>{t('title.search')}</h2>
          <p>{t('kw.subtitle')}</p>
        </div>
      </div>

      <div className="search-bar-container glass-panel">
        <div className="search-input-wrapper">
          <div className="search-field">
            <Search size={20} color="var(--text-muted)" />
            <input
              type="text"
              className="search-input"
              placeholder={t('kw.queryPlaceholder')}
              aria-label={t('kw.search')}
              value={query}
              maxLength={200}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={(e) => e.key === 'Enter' && !searching && handleSearch()}
            />
          </div>
          <button type="button" className="btn-primary search-btn" onClick={handleSearch} disabled={searching}>
            {searching ? t('kw.searching') : t('kw.search')}
          </button>
          {searching && searchContent && (
            <button type="button" className="btn-secondary search-stop" onClick={handleStop}>
              {t('kw.stop')}
            </button>
          )}
        </div>

        {regexError && (
          <InlineAlert variant="error">{regexError}</InlineAlert>
        )}

        <div className="search-filters">
          <span className="search-filters-title">
            <Filter size={16} /> {t('kw.filters')}
          </span>
          <label className="search-filter-label">
            {t('scan.category')}:
            <select
              value={category}
              disabled={searchContent}
              onChange={(e) => setCategory(e.target.value)}
              className="search-select"
            >
              {CATEGORIES.map((c) => (
                <option key={c.value || 'all'} value={c.value}>{t(c.labelKey)}</option>
              ))}
            </select>
          </label>
          {/* Single label wrapping the input — a nested <label htmlFor> made
              clicks on the text toggle the checkbox twice (net no-op). */}
          <label className="search-filter-label">
            <input
              type="checkbox"
              checked={searchContent}
              onChange={(e) => { setSearchContent(e.target.checked); }}
            />
            {t('kw.contentSearch')}
          </label>
          <label className="search-filter-label">
            <input
              type="checkbox"
              checked={useRegex}
              onChange={(e) => { setUseRegex(e.target.checked); setRegexError(''); }}
            />
            {t('kw.regex')}
          </label>
        </div>
      </div>

      <div className="search-results glass-panel">
        {searching && (
          <div className="examiner-empty" role="status" data-testid="search-progress">
            <Search size={48} className="spinner" aria-hidden="true" />
            <p>
              {searchContent && progress.total > 0
                ? tFormat('kw.contentProgress', { pct: String(progressPct), cur: String(progress.current), total: String(progress.total) })
                : t('kw.scanningFound')}
            </p>
          </div>
        )}

        {searchError && (
          <InlineAlert variant="error" testId="search-error">{searchError}</InlineAlert>
        )}
        {contentUnread && (
          <InlineAlert variant="warning" testId="content-search-unread">{t('kw.contentUnread')}</InlineAlert>
        )}

        {searchDone && !searchError && !regexError && results.length === 0 && (
          <div className="examiner-empty" role="status" data-testid="search-empty">
            <div className="examiner-icon" aria-hidden="true">
              <AlertCircle size={28} color="var(--warning-yellow)" />
            </div>
            <p className="empty-lead">{t('kw.noResultsLead')}<strong>{query}</strong>{t('kw.noResultsTail')}</p>
            <p className="empty-hint">{t('kw.tryDifferent')}</p>
          </div>
        )}

        {results.length > 0 && (
          <div className="results-list">
            <p className="search-count">{tFormat('kw.foundCount', { n: String(results.length) })}</p>
            <div className="results-stack">
              {results.map((r, i) => {
                const rec = r as SnippetRecord;
                const snippet = typeof rec.snippet === 'string' ? rec.snippet : '';
                return (
                  <div key={`${r.id}-${i}`} className="result-hit">
                    <FileText size={18} className="result-hit-ico" />
                    <span className="result-hit-name">
                      <HighlightText text={r.name} query={query.trim()} useRegex={useRegex && !searchContent} />
                    </span>
                    <span className="result-hit-path" title={r.path || undefined}>
                      {r.path ? <HighlightText text={r.path} query={query.trim()} useRegex={useRegex && !searchContent} /> : (r.category || '—')}
                    </span>
                    <span className="result-hit-size">
                      {r.sizeBytes ? (r.sizeBytes / 1024).toFixed(2) + ' KB' : ''}
                    </span>
                    {snippet && (
                      <div className="kw-snippet">
                        <SnippetText snippet={snippet} start={rec.snippetMatchStart} end={rec.snippetMatchEnd} />
                      </div>
                    )}
                  </div>
                );
              })}
            </div>
            {!searching && results.length >= 500 && (
              <p className="search-truncated">
                {t('kw.truncated')}
              </p>
            )}
          </div>
        )}

        {!searching && !searchDone && (
          <div className="examiner-empty" role="status" data-testid="search-prompt">
            <div className="examiner-icon" aria-hidden="true">
              <Keyboard size={28} />
            </div>
            <p>{t('kw.prompt')}</p>
          </div>
        )}
      </div>
    </div>
  );
};

export default KeywordSearch;
