import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

test('Byteback window shows', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    await expect(win).toHaveTitle(/Byteback/)
  } finally {
    await closeApp(launched)
  }
})

test('scan profile legend and mode buttons', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    const legend = win.getByTestId('scan-profile-legend')
    await expect(legend).toBeVisible()
    await expect(win.getByTestId('dash-session-load-error')).toHaveCount(0)
    await expect(win.getByTestId('dash-volume-letters-error')).toHaveCount(0)
    await expect(win.getByTestId('engine-load-error')).toHaveCount(0)
    await expect(win.getByTestId('scan-hydrate-error')).toHaveCount(0)
    await expect(win.getByTestId('session-log-unread-error')).toHaveCount(0)
    await expect(legend).toContainText('Hızlı')
    await expect(legend).toContainText('Derin')
    await expect(legend).toContainText('Tam disk carve')
    await expect(legend).toContainText('boş')

    const quickBtn = win.getByTestId('scan-mode-quick')
    if (await quickBtn.count()) {
      await expect(quickBtn.first()).toBeVisible()
      await expect(win.getByTestId('scan-mode-deep').first()).toBeVisible()
      await expect(win.getByTestId('scan-mode-full-carve').first()).toBeVisible()
    }

    await expect(win.getByTestId('examiner-statusbar')).toBeVisible()
    await expect(win.getByTestId('skip-to-main')).toHaveAttribute('href', '#main-content')
    await expect(win.getByTestId('nav-dashboard')).toBeVisible()
    await expect(win.getByTestId('nav-case')).toBeVisible()
    await win.getByTestId('nav-hex').click()
    await expect(win.getByTestId('hex-empty')).toBeVisible()
    await expect(win.getByTestId('hex-empty')).toHaveAttribute('role', 'status')
    await expect(win.getByTestId('hex-empty')).toContainText('Sürücü')
    await expect(win.getByTestId('hex-raid-state-error')).toHaveCount(0)
    await win.getByTestId('nav-imager').click()
    await expect(win.getByTestId('imager-use-volume')).toHaveCount(0)
    await expect(win.getByTestId('imager-raid-source')).toHaveCount(0)
    await expect(win.getByTestId('imager-form-error')).toHaveCount(0)
    await expect(win.getByTestId('imager-raid-state-error')).toHaveCount(0)
    const imagerEmpty = win.getByTestId('imager-empty')
    if (await imagerEmpty.count()) {
      await expect(imagerEmpty).toHaveAttribute('role', 'status')
    }
    await win.getByTestId('nav-smart').click()
    await expect(win.getByTestId('smart-empty')).toBeVisible()
    await expect(win.getByTestId('smart-empty')).toHaveAttribute('role', 'status')
    await win.getByTestId('nav-raid').click()
    await expect(win.locator('#main-content').getByRole('heading', { name: 'Sanal RAID Oluşturucu (Virtual RAID Constructor)' })).toBeVisible()
    await expect(win.getByTestId('raid-empty-array')).toBeVisible()
    await expect(win.getByTestId('raid-empty-array')).toHaveAttribute('role', 'status')
    await expect(win.getByTestId('raid-stripe-select')).toBeVisible()
    await expect(win.getByTestId('raid-offset-sectors')).toBeVisible()
    await win.getByTestId('nav-case').click()
    await expect(win.getByTestId('evidence-paths')).toBeVisible()
    await expect(win.getByTestId('case-load-error')).toHaveCount(0)
    await expect(win.getByTestId('case-nsrl-error')).toHaveCount(0)
    await expect(win.getByTestId('nav-results')).toBeDisabled()
    await expect(win.getByTestId('nav-search')).toBeDisabled()
    await expect(win.getByTestId('nav-timeline')).toBeDisabled()
    await expect(win.getByTestId('nav-report')).toBeDisabled()
    await win.getByTestId('nav-shredder').click()
    await expect(win.locator('#main-content').getByRole('heading', { name: 'Veri Yok Edici' })).toBeVisible()
  } finally {
    await closeApp(launched)
  }
})
