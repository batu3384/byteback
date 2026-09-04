import { useSyncExternalStore } from 'react'

// CA-053: minimal framework-free i18n. TR is the source language; EN is the
// second locale. Missing keys fall back to the key itself so untranslated
// surfaces stay visible instead of silently blank.
// ponytail: only the primary recovery flow (chrome, dashboard, scan, results)
// is migrated; expert pages still render their hardcoded TR strings until
// their keys land here.

export type Lang = 'tr' | 'en'

const LANG_KEY = 'byteback-lang'

let lang: Lang = 'tr'
try {
  const stored = localStorage.getItem(LANG_KEY)
  if (stored === 'tr' || stored === 'en') lang = stored
} catch {
  /* storage unavailable -> default */
}

const listeners = new Set<() => void>()

export function setLang(next: Lang): void {
  if (next === lang) return
  lang = next
  try {
    localStorage.setItem(LANG_KEY, next)
  } catch {
    /* storage unavailable */
  }
  listeners.forEach((fn) => fn())
}

type Entry = { tr: string; en: string }

const STRINGS: Record<string, Entry> = {
  // Chrome
  'nav.dashboard': { tr: 'Ana Ekran', en: 'Dashboard' },
  'nav.scan': { tr: 'Aktif Tarama', en: 'Active Scan' },
  'nav.results': { tr: 'Sonuçlar', en: 'Results' },
  'nav.search': { tr: 'Kelime Arama', en: 'Keyword Search' },
  'nav.hex': { tr: 'Hex İnceleyici', en: 'Hex Viewer' },
  'nav.timeline': { tr: 'Zaman Çizelgesi', en: 'Timeline' },
  'nav.report': { tr: 'Adli Rapor', en: 'Forensic Report' },
  'nav.imager': { tr: 'İmaj (RAW / E01)', en: 'Image (RAW / E01)' },
  'nav.smart': { tr: 'S.M.A.R.T.', en: 'S.M.A.R.T.' },
  'nav.raid': { tr: 'Sanal RAID', en: 'Virtual RAID' },
  'nav.case': { tr: 'Dava / NSRL', en: 'Case / NSRL' },
  'nav.shredder': { tr: 'Veri Yok Edici', en: 'Data Shredder' },
  'nav.groupTitle.recovery': { tr: 'Kurtarma', en: 'Recovery' },
  'nav.groupTitle.inspect': { tr: 'İnceleme', en: 'Inspection' },
  'nav.groupTitle.expert': { tr: 'Disk / uzman', en: 'Disk / expert' },
  'header.scanRunning': { tr: 'Tarama sürüyor', en: 'Scan running' },
  'header.toLightTheme': { tr: 'Açık temaya geç', en: 'Switch to light theme' },
  'header.toDarkTheme': { tr: 'Koyu temaya geç', en: 'Switch to dark theme' },
  'header.toEnglish': { tr: 'English', en: 'Türkçe' },
  'title.dashboard': { tr: 'Ana Ekran', en: 'Dashboard' },
  'title.scan': { tr: 'Aktif Tarama', en: 'Active Scan' },
  'title.results': { tr: 'Kurtarma Sonuçları', en: 'Recovery Results' },
  'title.hex': { tr: 'Hex İnceleyici', en: 'Hex Viewer' },
  'title.imager': { tr: 'İmaj (RAW / E01)', en: 'Image (RAW / E01)' },
  'title.smart': { tr: 'S.M.A.R.T. Durumu', en: 'S.M.A.R.T. Status' },
  'title.search': { tr: 'Kelime Arama', en: 'Keyword Search' },
  'title.report': { tr: 'Adli Rapor', en: 'Forensic Report' },
  'title.shredder': { tr: 'Veri Yok Edici', en: 'Data Shredder' },
  'title.raid': { tr: 'Sanal RAID Oluştur', en: 'Create Virtual RAID' },
  'title.timeline': { tr: 'Olay Zaman Çizelgesi', en: 'Event Timeline' },
  'title.case': { tr: 'Dava / NSRL', en: 'Case / NSRL' },
  'nav.reportLockedTitle': { tr: 'Adli rapor için tamamlanmış tarama gerekli', en: 'A completed scan is required for a forensic report' },
  'nav.scanLockedTitle': { tr: 'Önce bir taramayı tamamlayın veya duraklatılmış oturumu açın', en: 'Complete a scan or open a paused session first' },
  'nav.diskBusyTitle': { tr: 'Hex, imaj ve imha tarama bitene kadar kapalı', en: 'Hex, imaging and shredder stay closed until the scan finishes' },

  // Dashboard
  'dash.volumeScanTitle': { tr: 'Mantıksal sürücüden tara', en: 'Scan from a logical volume' },
  'dash.volumeScanHint': { tr: 'Harf (ör. D:) → PhysicalDrive + bölüm ofseti. Yalnızca o birimi tarar.', en: 'Letter (e.g. D:) → PhysicalDrive + partition offset. Scans only that volume.' },
  'dash.volumeLetterLabel': { tr: 'Mantıksal sürücü harfi', en: 'Logical drive letter' },
  'dash.pausedTitle': { tr: 'Yarım Kalan Tarama', en: 'Unfinished Scan' },
  'dash.viewPausedResults': { tr: 'Sonuçları gör', en: 'View results' },
  'dash.resume': { tr: 'Devam et', en: 'Resume' },
  'dash.scanRunningTitle': { tr: 'Tarama sürüyor', en: 'Scan running' },
  'profile.quick.label': { tr: 'Hızlı', en: 'Quick' },
  'profile.deep.label': { tr: 'Derin', en: 'Deep' },
  'profile.carve_only.label': { tr: 'Yalnız carve', en: 'Carve only' },
  'profile.full_carve.label': { tr: 'Tam disk carve', en: 'Full disk carve' },

  // ScanView
  'scan.titleRunning': { tr: 'taranıyor', en: 'being scanned' },
  'scan.raid': { tr: 'RAID', en: 'RAID' },
  'scan.finished': { tr: 'Tarama tamamlandı', en: 'Scan complete' },
  'scan.cancelled': { tr: 'Tarama iptal edildi', en: 'Scan cancelled' },
  'scan.paused': { tr: 'Tarama duraklatıldı', en: 'Scan paused' },
  'scan.failed': { tr: 'Tarama başarısız', en: 'Scan failed' },
  'scan.records': { tr: 'Kayıt', en: 'Records' },
  'scan.deletedShort': { tr: 'silinmiş', en: 'deleted' },
  'scan.carvedShort': { tr: 'oyulmuş', en: 'carved' },
  'scan.elapsed': { tr: 'Geçen Süre', en: 'Elapsed' },
  'scan.remaining': { tr: 'Kalan Süre', en: 'Remaining' },
  'scan.deleted': { tr: 'Silinmiş', en: 'Deleted' },
  'scan.prev': { tr: 'Önceki', en: 'Previous' },
  'scan.next': { tr: 'Sonraki', en: 'Next' },
  'scan.all': { tr: 'Tümü', en: 'All' },
  'scan.image': { tr: 'Resim', en: 'Image' },
  'scan.video': { tr: 'Video', en: 'Video' },
  'scan.audio': { tr: 'Ses', en: 'Audio' },
  'scan.document': { tr: 'Belge', en: 'Document' },
  'scan.archive': { tr: 'Arşiv', en: 'Archive' },
  'scan.stop': { tr: 'Taramayı Durdur', en: 'Stop Scan' },
  'scan.viewResults': { tr: 'Sonuçları Görüntüle', en: 'View Results' },
  'scan.backHome': { tr: 'Ana ekran', en: 'Dashboard' },
  'scan.stopping': { tr: 'Native tarama duruyor…', en: 'Native scan stopping…' },
  'scan.fileDetail': { tr: 'Dosya Detayı', en: 'File Detail' },
  'scan.category': { tr: 'Kategori', en: 'Category' },
  'scan.size': { tr: 'Boyut', en: 'Size' },
  'scan.startSector': { tr: 'Başlangıç Sektörü', en: 'Start Sector' },
  'scan.endSector': { tr: 'Bitiş Sektörü', en: 'End Sector' },
  'scan.confidence': { tr: 'Güven Skoru', en: 'Confidence' },
  'scan.statusLabel': { tr: 'Durum', en: 'Status' },
  'scan.source': { tr: 'Kaynak', en: 'Source' },
  'scan.runCount': { tr: 'Data Run Sayısı', en: 'Data Runs' },
  'scan.created': { tr: 'Oluşturma', en: 'Created' },
  'scan.modified': { tr: 'Değiştirme', en: 'Modified' },
  'scan.path': { tr: 'Yol', en: 'Path' },
  'scan.noFiles': { tr: 'Henüz dosya bulunamadı...', en: 'No files found yet...' },
  'scan.sector': { tr: 'Sektör', en: 'Sector' },

  // ResultsView
  'results.title': { tr: 'Kurtarma Sonuçları', en: 'Recovery Results' },
  'results.inFilter': { tr: 'Bu süzgeçte', en: 'In this filter' },
  'results.files': { tr: 'dosya', en: 'files' },
  'results.page': { tr: 'sayfa', en: 'page' },
  'results.deleted': { tr: 'Silinmiş', en: 'Deleted' },
  'results.allocated': { tr: 'Tahsisli', en: 'Allocated' },
  'results.carved': { tr: 'Oyulmuş', en: 'Carved' },
  'results.total': { tr: 'Toplam', en: 'Total' },
  'results.preview': { tr: 'Önizle', en: 'Preview' },
  'results.previewing': { tr: 'Önizleniyor...', en: 'Previewing...' },
  'results.exportCsv': { tr: 'Dışa Aktar (CSV)', en: 'Export (CSV)' },
  'results.exporting': { tr: 'Aktarılıyor…', en: 'Exporting…' },
  'results.recoverSelected': { tr: 'Seçilenleri Kurtar', en: 'Recover Selected' },
  'results.recovering': { tr: 'Kurtarılıyor...', en: 'Recovering...' },
  'results.statusFilter': { tr: 'Durum', en: 'Status' },
  'results.typeFilter': { tr: 'Tip', en: 'Type' },
  'results.all': { tr: 'Tümü', en: 'All' },
  'results.duplicates': { tr: 'Tekrarlar', en: 'Duplicates' },
  'results.searchByName': { tr: 'Ada göre ara', en: 'Search by name' },
  'results.gallery': { tr: 'Galeri', en: 'Gallery' },
  'results.list': { tr: 'Liste', en: 'List' },
  'results.tree': { tr: 'Ağaç', en: 'Tree' },
  'results.col.name': { tr: 'Dosya Adı', en: 'Name' },
  'results.col.size': { tr: 'Boyut', en: 'Size' },
  'results.col.date': { tr: 'Değiştirilme', en: 'Modified' },
  'results.col.confidence': { tr: 'Güven', en: 'Confidence' },
  'results.col.source': { tr: 'Kaynak', en: 'Source' },
  'results.col.status': { tr: 'Durum', en: 'Status' },
  'results.empty': { tr: 'Bu süzgeçte dosya yok.', en: 'No files in this filter.' },
  'results.loading': { tr: 'Yükleniyor…', en: 'Loading…' },
  'results.selectAllPage': { tr: 'Bu sayfadaki tümünü seç', en: 'Select all on this page' },
  'results.prev': { tr: 'Önceki', en: 'Previous' },
  'results.next': { tr: 'Sonraki', en: 'Next' },
  'results.pageLabel': { tr: 'Sayfa', en: 'Page' },
  'results.noPreview': { tr: 'önizlenemedi', en: 'no preview' },
  'results.noImages': { tr: 'Bu sayfada resim yok. Galeri yalnızca geçerli sayfadaki resimleri gösterir.', en: 'No images on this page. The gallery only shows images on the current page.' },
}

export function t(key: string): string {
  const entry = STRINGS[key]
  return entry ? entry[lang] : key
}

export function getLang(): Lang {
  return lang
}

/** Subscribe React components to language changes. */
export function useI18n(): { lang: Lang; t: (key: string) => string; setLang: (l: Lang) => void } {
  useSyncExternalStore(
    (cb) => {
      listeners.add(cb)
      return () => listeners.delete(cb)
    },
    () => lang,
  )
  return { lang, t, setLang }
}
