/** Structured IPC results for examiner-visible errors (avoid silent null/false). */

export type HexReadResult = { data: number[] | null; error?: string }

export type HexSearchResult = { hits: number[]; unread: boolean; error?: string }

export type MftAttrSummary = { type: number; name: string; resident: boolean }

export type MftRecordResult = {
  ok: boolean
  unread: boolean
  mftRef?: number
  byteOffset?: number
  signature?: string
  flags?: number
  attrs?: MftAttrSummary[]
  error?: string
}

export type IpcOkResult = { ok: boolean; error?: string }

export function ipcOk(ok: boolean, error?: string): IpcOkResult {
  return error ? { ok, error } : { ok }
}
