import { describe, expect, it } from 'vitest'
import { defaultImagerUseVolume, isWin32VolumeDevicePath } from './win32-volume-path'

describe('isWin32VolumeDevicePath', () => {
  it('accepts drive-letter volume devices', () => {
    expect(isWin32VolumeDevicePath('\\\\.\\C:')).toBe(true)
    expect(isWin32VolumeDevicePath('\\\\.\\d:')).toBe(true)
  })

  it('rejects PhysicalDrive, UNC, and trailing slash', () => {
    expect(isWin32VolumeDevicePath('\\\\.\\PhysicalDrive0')).toBe(false)
    expect(isWin32VolumeDevicePath('\\\\.\\C:\\')).toBe(false)
    expect(isWin32VolumeDevicePath('\\\\server\\share')).toBe(false)
    expect(isWin32VolumeDevicePath('C:')).toBe(false)
    expect(isWin32VolumeDevicePath('')).toBe(false)
  })
})

describe('defaultImagerUseVolume', () => {
  it('defaults on for a scan-bound volume device', () => {
    expect(defaultImagerUseVolume('\\\\.\\E:')).toBe(true)
  })

  it('stays off without a valid volume bind', () => {
    expect(defaultImagerUseVolume(undefined)).toBe(false)
    expect(defaultImagerUseVolume('')).toBe(false)
    expect(defaultImagerUseVolume('\\\\.\\PhysicalDrive0')).toBe(false)
  })
})
