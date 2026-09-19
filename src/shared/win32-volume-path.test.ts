import { describe, expect, it } from 'vitest'
import { defaultImagerUseVolume, imagerStartArgs, isEvidenceImagePath, isWin32VolumeDevicePath } from './win32-volume-path'

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

describe('isEvidenceImagePath', () => {
  it('accepts a local file path', () => {
    expect(isEvidenceImagePath('C:\\cases\\disk.E01')).toBe(true)
    expect(isEvidenceImagePath('/tmp/disk.img')).toBe(true)
  })

  it('rejects devices, http, and empty', () => {
    expect(isEvidenceImagePath('\\\\.\\C:')).toBe(false)
    expect(isEvidenceImagePath('\\\\.\\PhysicalDrive0')).toBe(false)
    expect(isEvidenceImagePath('http://host/disk.img')).toBe(false)
    expect(isEvidenceImagePath('https://host/disk.E01')).toBe(false)
    expect(isEvidenceImagePath('/dev/sda')).toBe(false)
    expect(isEvidenceImagePath('')).toBe(false)
    expect(isEvidenceImagePath(undefined)).toBe(false)
  })
})

describe('defaultImagerUseVolume', () => {
  it('defaults on for a scan-bound volume device', () => {
    expect(defaultImagerUseVolume('\\\\.\\E:')).toBe(true)
  })

  it('defaults on for a scan-bound evidence image', () => {
    expect(defaultImagerUseVolume('C:\\cases\\disk.img')).toBe(true)
  })

  it('stays off without a valid volume bind', () => {
    expect(defaultImagerUseVolume(undefined)).toBe(false)
    expect(defaultImagerUseVolume('')).toBe(false)
    expect(defaultImagerUseVolume('\\\\.\\PhysicalDrive0')).toBe(false)
    expect(defaultImagerUseVolume('http://host/disk.img')).toBe(false)
  })
})

describe('imagerStartArgs', () => {
  it('binds evidence image as driveIndex -2', () => {
    expect(imagerStartArgs('', 'D:\\out.dd', true, 'C:\\cases\\disk.img')).toEqual({
      driveIndex: -2,
      volumePath: 'C:\\cases\\disk.img',
    })
  })

  it('keeps PhysicalDrive plus volume device', () => {
    expect(imagerStartArgs(2, 'D:\\out.dd', true, '\\\\.\\E:')).toEqual({
      driveIndex: 2,
      volumePath: '\\\\.\\E:',
    })
  })

  it('requires a drive when no evidence bind', () => {
    expect(imagerStartArgs('', 'D:\\out.dd', false, 'C:\\cases\\disk.img')).toBeNull()
  })
})
