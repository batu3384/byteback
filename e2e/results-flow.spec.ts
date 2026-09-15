import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

const FIXTURE_FILES = [
  { name: 'vacation.jpg', path: '/recovered_raw/vacation.jpg', sizeBytes: 4_000_000, confidence: 95, status: 0, source: 'carver', category: 'Image', startSector: 100, endSector: 9000 },
  { name: 'family.png', path: '/recovered_raw/family.png', sizeBytes: 2_000_000, confidence: 88, status: 0, source: 'carver', category: 'Image', startSector: 10000, endSector: 14000 },
  { name: 'shaky.jpg', path: '/recovered_raw/shaky.jpg', sizeBytes: 500_000, confidence: 35, status: 0, source: 'carver', category: 'Image', startSector: 20000, endSector: 21000 },
  { name: 'report.docx', path: 'Users/bat/Documents/report.docx', sizeBytes: 120_000, confidence: 90, status: 0, source: 'ntfs_mft', category: 'Document', startSector: 30000, endSector: 30100 },
  { name: 'clip.mp4', path: '/recovered_raw/clip.mp4', sizeBytes: 900_000_000, confidence: 90, status: 0, source: 'carver', category: 'Video', startSector: 40000, endSector: 1800000 },
]

test('seeded scan flows into results triage and unlocks the report nav', async () => {
  const launched = await launchApp()
  const { app, win } = launched

  const scanId = await win.evaluate((files) => window.api.seedScanFixture(files), FIXTURE_FILES)
  expect(scanId).toBeGreaterThan(0)

  // Fresh hydration: the app picks up the latest usable scan on startup.
  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

  // Completed scan unlocks the forensic report nav.
  await expect(win.getByTestId('nav-report')).toBeEnabled()
  await expect(win.getByTestId('nav-results')).toBeEnabled()

  await win.getByTestId('nav-results').click()

  // Default filter is "deleted" (metadata only): the single MFT record.
  await expect(win.getByTestId('result-row')).toHaveCount(1)
  await win.getByTestId('filter-all').click()
  await expect(win.getByTestId('result-row')).toHaveCount(5)

  // Confidence column renders tiered chips.
  await expect(win.getByTestId('result-row').first()).toContainText('vacation.jpg')
  await expect(win.getByTestId('confidence-95')).toBeVisible()
  await expect(win.getByTestId('confidence-35')).toBeVisible()

  // Status filter narrows to carved records only.
  await win.getByTestId('filter-carved').click()
  await expect(win.getByTestId('result-row')).toHaveCount(4)
  await win.getByTestId('filter-deleted').click()
  await expect(win.getByTestId('result-row')).toHaveCount(1)
  await win.getByTestId('filter-all').click()
  await expect(win.getByTestId('result-row')).toHaveCount(5)

  // Name search filters live.
  await win.getByLabel('Dosya adı ara').fill('vacation')
  await expect(win.getByTestId('result-row')).toHaveCount(1)
  await win.getByLabel('Dosya adı ara').fill('zzz-no-such-file')
  await expect(win.getByTestId('results-empty')).toBeVisible()
  await expect(win.getByTestId('results-empty')).toHaveAttribute('role', 'status')
  await expect(win.getByTestId('results-empty')).toContainText('süzgeçte')
  await expect(win.getByTestId('results-list-error')).toHaveCount(0)
  await win.getByLabel('Dosya adı ara').fill('')
  await expect(win.getByTestId('result-row')).toHaveCount(5)

  // Size/date filter inputs (P0-4) narrow the page live. Fixture sizes:
  // 4MB, 2MB, 0.5MB, 0.12MB and 900MB — min 5MB leaves only the video.
  await win.getByLabel('En küçük boyut MB').fill('5')
  await expect(win.getByTestId('result-row')).toHaveCount(1)
  await win.getByRole('button', { name: 'Süzgeçleri temizle' }).click()
  await expect(win.getByTestId('result-row')).toHaveCount(5)

  // Preserve-paths checkbox (P0-1) toggles next to the recover button.
  await expect(win.getByTestId('preserve-paths')).toBeChecked()

  // Selection counter follows the checkbox.
  await win.locator('tbody input[type="checkbox"]').first().check()
  await expect(win.getByRole('button', { name: /Seçilenleri Kurtar \(1\)/ })).toBeVisible()

  await closeApp(launched)
})

test('language toggle switches the sidebar labels and back', async () => {
  const launched = await launchApp()
  const { win } = launched
  await expect(win.getByTestId('nav-results')).toContainText('Sonuçlar')
  await win.getByRole('button', { name: 'English' }).click()
  await expect(win.getByTestId('nav-results')).toContainText('Results')
  // Restore Turkish so the persisted choice does not leak into other tests.
  await win.getByRole('button', { name: 'Türkçe' }).click()
  await expect(win.getByTestId('nav-results')).toContainText('Sonuçlar')
  await closeApp(launched)
})

test('report page shows the audit chain verification status', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'evidence.docx', path: 'Users/bat/Documents/evidence.docx', sizeBytes: 120_000, confidence: 90, status: 0, source: 'ntfs_mft', category: 'Document', startSector: 100, endSector: 300 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

  // Completed scan unlocks the report page.
  await expect(win.getByTestId('nav-report')).toBeEnabled()
  await win.getByTestId('nav-report').click()

  // The custody/verify card must resolve to a real chain verdict, not stay
  // stuck on "Doğrulanıyor…".
  const status = win.getByTestId('report-chain-status')
  await expect(status).toBeVisible()
  await expect(status).toContainText(/Doğrulandı|BOZULDU/)
  await expect(win.getByTestId('report-empty')).toBeVisible()
  await expect(win.getByTestId('report-empty')).toHaveAttribute('role', 'status')

  await closeApp(launched)
})

test('seeded ReFS probe-cap record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'evidence.docx', path: 'Users/bat/Documents/evidence.docx', sizeBytes: 120_000, confidence: 90, status: 0, source: 'ntfs_mft', category: 'Document', startSector: 100, endSector: 300 },
    { name: 'ReFS_Volume', path: '/refs-probe-capped/', sizeBytes: 0, confidence: 35, status: 0, source: 'refs_volume', category: 'System', startSector: 0, endSector: 1 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('refs-probe-capped')).toBeVisible()
  await expect(win.getByTestId('refs-probe-capped')).toContainText('ReFS')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded ReFS SUPB-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'report.docx', path: '/report.docx', sizeBytes: 0, confidence: 65, status: 0, source: 'refs_volume', category: 'Document', startSector: 8, endSector: 16 },
    { name: 'Refs_SupbUnread', path: '/refs-supb-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'refs_supb_unread', category: 'System', startSector: 240, endSector: 248 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('refs-supb-unread')).toBeVisible()
  await expect(win.getByTestId('refs-supb-unread')).toContainText('SUPB')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded ReFS ministore-page-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'Refs_PageUnread', path: '/refs-page-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'refs_page_unread', category: 'System', startSector: 8, endSector: 16 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('refs-supb-unread')).toBeVisible()
  await expect(win.getByTestId('refs-supb-unread')).toContainText('ministore')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded FAT directory-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'photo.jpg', path: 'DCIM/photo.jpg', sizeBytes: 80_000, confidence: 90, status: 0, source: 'fat', category: 'Image', startSector: 40, endSector: 80 },
    { name: 'FAT_ChainUnread', path: '/fat-chain-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'fat_chain_unread', category: 'System', startSector: 1, endSector: 2 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('fat-dir-unread')).toBeVisible()
  await expect(win.getByTestId('fat-dir-unread')).toContainText('FAT')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded ext4 directory-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'note.txt', path: '/note.txt', sizeBytes: 12, confidence: 95, status: 1, source: 'ext4_dirent', category: 'File', startSector: 14, endSector: 16 },
    { name: 'Ext4_DirectoryUnread', path: '/ext4-dir-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'ext4_dir_unread', category: 'System', startSector: 12, endSector: 14 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('ext4-dir-unread')).toBeVisible()
  await expect(win.getByTestId('ext4-dir-unread')).toContainText('extent')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded XFS directory-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'doc', path: '/doc', sizeBytes: 512, confidence: 90, status: 1, source: 'xfs_inode', category: 'File', startSector: 50, endSector: 51 },
    { name: 'Xfs_DirectoryUnread', path: '/xfs-dir-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'xfs_dir_unread', category: 'System', startSector: 20, endSector: 21 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('xfs-dir-unread')).toBeVisible()
  await expect(win.getByTestId('xfs-dir-unread')).toContainText('XFS')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded XFS superblock-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'alfa', path: '/alfa', sizeBytes: 4096, confidence: 90, status: 1, source: 'xfs_inode', category: 'File', startSector: 60, endSector: 61 },
    { name: 'Xfs_SuperblockUnread', path: '/xfs-sb-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'xfs_sb_unread', category: 'System', startSector: 64, endSector: 65 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('xfs-dir-unread')).toBeVisible()
  await expect(win.getByTestId('xfs-dir-unread')).toContainText('XFS')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded XFS inode-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'Xfs_InodeUnread', path: '/xfs-inode-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'xfs_inode_unread', category: 'System', startSector: 66, endSector: 67 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('xfs-dir-unread')).toBeVisible()
  await expect(win.getByTestId('xfs-dir-unread')).toContainText('inode')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded XFS bmap-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'big', path: '/big', sizeBytes: 2048, confidence: 90, status: 1, source: 'xfs_inode', category: 'File', startSector: 40, endSector: 42 },
    { name: 'Xfs_BmapUnread', path: '/xfs-bmap-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'xfs_bmap_unread', category: 'System', startSector: 30, endSector: 31 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('xfs-dir-unread')).toBeVisible()
  await expect(win.getByTestId('xfs-dir-unread')).toContainText('bmap')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded HFS catalog-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'Hfs_CatalogUnread', path: '/hfs-catalog-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'hfs_catalog_unread', category: 'System', startSector: 32, endSector: 40 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('hfs-catalog-unread')).toBeVisible()
  await expect(win.getByTestId('hfs-catalog-unread')).toContainText('katalog')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded HFS linear-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'Hfs_LinearUnread', path: '/hfs-linear-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'hfs_linear_unread', category: 'System', startSector: 0, endSector: 1 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('hfs-catalog-unread')).toBeVisible()
  await expect(win.getByTestId('hfs-catalog-unread')).toContainText('doğrusal')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded APFS NXSB-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'Macintosh HD', path: '/apfs/Macintosh HD', sizeBytes: 4096, confidence: 90, status: 0, source: 'apfs_volume', category: 'System', startSector: 8, endSector: 16 },
    { name: 'Apfs_NxsbUnread', path: '/apfs-nxsb-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'apfs_nxsb_unread', category: 'System', startSector: 0, endSector: 8 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('apfs-nxsb-unread')).toBeVisible()
  await expect(win.getByTestId('apfs-nxsb-unread')).toContainText('NXSB')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded APFS block-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'APFS_Container', path: '/apfs/', sizeBytes: 131072, confidence: 95, status: 0, source: 'apfs_container', category: 'System', startSector: 0, endSector: 256 },
    { name: 'Apfs_BlockUnread', path: '/apfs-block-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'apfs_block_unread', category: 'System', startSector: 0, endSector: 1 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('apfs-nxsb-unread')).toBeVisible()
  await expect(win.getByTestId('apfs-nxsb-unread')).toContainText('APSB')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded APFS linear-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'Apfs_LinearUnread', path: '/apfs-linear-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'apfs_linear_unread', category: 'System', startSector: 0, endSector: 1 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('apfs-nxsb-unread')).toBeVisible()
  await expect(win.getByTestId('apfs-nxsb-unread')).toContainText('doğrusal')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded NTFS $I30-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'Dir', path: '/Dir', sizeBytes: 0, confidence: 80, status: 1, source: 'ntfs_mft', category: 'Folder', startSector: 16, endSector: 18 },
    { name: 'Ntfs_I30Unread', path: '/ntfs-i30-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'ntfs_i30_unread', category: 'System', startSector: 24, endSector: 32 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('ntfs-i30-unread')).toBeVisible()
  await expect(win.getByTestId('ntfs-i30-unread')).toContainText('$I30')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded unallocated-map-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'note.txt', path: '/note.txt', sizeBytes: 12, confidence: 95, status: 1, source: 'ext4_dirent', category: 'File', startSector: 14, endSector: 16 },
    { name: 'Unalloc_MapUnread', path: '/unalloc-map-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'unalloc_map_unread', category: 'System', startSector: 8, endSector: 10 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('unalloc-map-unread')).toBeVisible()
  await expect(win.getByTestId('unalloc-map-unread')).toContainText('bitmap')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded NTFS $LogFile-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'alive.bin', path: '/alive.bin', sizeBytes: 64, confidence: 90, status: 1, source: 'ntfs_mft', category: 'File', startSector: 16, endSector: 18 },
    { name: 'Ntfs_LogfileUnread', path: '/ntfs-logfile-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'ntfs_logfile_unread', category: 'System', startSector: 2, endSector: 4 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('ntfs-logfile-unread')).toBeVisible()
  await expect(win.getByTestId('ntfs-logfile-unread')).toContainText('$LogFile')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded USN-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'alive.bin', path: '/alive.bin', sizeBytes: 64, confidence: 90, status: 1, source: 'ntfs_mft', category: 'File', startSector: 16, endSector: 18 },
    { name: 'Ntfs_UsnUnread', path: '/usn-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'usn_unread', category: 'System', startSector: 32, endSector: 40 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('usn-unread')).toBeVisible()
  await expect(win.getByTestId('usn-unread')).toContainText('$UsnJrnl')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded NTFS $MFT-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'alive.bin', path: '/alive.bin', sizeBytes: 64, confidence: 90, status: 1, source: 'ntfs_mft', category: 'File', startSector: 16, endSector: 18 },
    { name: 'Ntfs_MftUnread', path: '/ntfs-mft-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'ntfs_mft_unread', category: 'System', startSector: 8, endSector: 10 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('ntfs-mft-unread')).toBeVisible()
  await expect(win.getByTestId('ntfs-mft-unread')).toContainText('$MFT')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded probe-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'alive.bin', path: '/alive.bin', sizeBytes: 64, confidence: 90, status: 1, source: 'ntfs_mft', category: 'File', startSector: 16, endSector: 18 },
    { name: 'Probe_Unread', path: '/probe-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'probe_unread', category: 'System', startSector: 0, endSector: 1 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('probe-unread')).toBeVisible()
  await expect(win.getByTestId('probe-unread')).toContainText('bölüm tablosu')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded carver-unread record surfaces the honesty banner on results', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'photo.png', path: '/photo.png', sizeBytes: 4096, confidence: 80, status: 2, source: 'carver', category: 'Image', startSector: 8, endSector: 16 },
    { name: 'Carver_Unread', path: '/carver-unread/', sizeBytes: 0, confidence: 20, status: 0, source: 'carver_unread', category: 'System', startSector: 0, endSector: 1 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-results')).toBeEnabled()
  await win.getByTestId('nav-results').click()
  await expect(win.getByTestId('carver-unread')).toBeVisible()
  await expect(win.getByTestId('carver-unread')).toContainText('oyma')
  await expect(win.getByTestId('results-honesty-load-error')).toHaveCount(0)

  await closeApp(launched)
})

test('seeded scan unlocks search with an examiner empty prompt', async () => {
  const launched = await launchApp()
  const { win } = launched

  const scanId = await win.evaluate(() => window.api.seedScanFixture([
    { name: 'note.txt', path: '/note.txt', sizeBytes: 32, confidence: 90, status: 0, source: 'ntfs_mft', category: 'Document', startSector: 1, endSector: 2 },
  ]))
  expect(scanId).toBeGreaterThan(0)

  await win.reload()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  await expect(win.getByTestId('nav-search')).toBeEnabled()
  await win.getByTestId('nav-search').click()
  await expect(win.getByTestId('search-prompt')).toBeVisible()
  await expect(win.getByTestId('search-prompt')).toHaveAttribute('role', 'status')
  await expect(win.getByTestId('content-search-unread')).toHaveCount(0)

  await closeApp(launched)
})
