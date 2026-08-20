/** Structured IPC results for examiner-visible errors (avoid silent null/false). */

export type HexReadResult = { data: number[] | null; error?: string }

export type IpcOkResult = { ok: boolean; error?: string }

export function ipcOk(ok: boolean, error?: string): IpcOkResult {
  return error ? { ok, error } : { ok }
}
