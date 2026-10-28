import { hasValidScanId } from './scan-required'

export interface AppErrorBoundaryCopy {
  body: string
  primaryLabel: string
  useWindowReload: boolean
}

/** Audit Bölüm 6: no stack to the operator; scan-live vs idle messaging. */
export function appErrorBoundaryCopy(scanLive: boolean, scanId: number): AppErrorBoundaryCopy {
  if (scanLive) {
    return {
      body:
        'A panel failed while a scan may still be running. Reload the window; progress is recorded in the session log.',
      primaryLabel: 'Reload window',
      useWindowReload: true,
    }
  }
  const scanHint = hasValidScanId(scanId) ? ` Last scan id on disk: ${scanId}.` : ''
  return {
    body: `The examiner surface hit an unexpected error.${scanHint} Case data on disk is unchanged.`,
    primaryLabel: 'Try again',
    useWindowReload: false,
  }
}
