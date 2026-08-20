/** Wrap native engine calls so IPC handlers fail loudly instead of masking errors. */

export function callNative<T>(channel: string, fn: () => T): T {
  try {
    return fn()
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    console.error(`[IPC] ${channel} error:`, err)
    throw new Error(`Native engine error (${channel}): ${msg}`)
  }
}
