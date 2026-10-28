import { getEngine } from './native-bridge'

/** ASCII-only sanitizer for hash-chain audit lines (native line format is ASCII). */
export function asciiForAudit(text: string): string {
  return text.replace(/[^\x20-\x7E]/g, '?').slice(0, 200)
}

/** Best-effort write into the native hash-chained audit log. Never throws. */
export function auditChainEvent(line: string): void {
  try {
    const engine = getEngine() as unknown as { logAuditEvent?: (e: string) => boolean }
    engine.logAuditEvent?.(line)
  } catch (err) {
    console.warn('[IPC] auditChainEvent failed:', err instanceof Error ? err.message : err)
  }
}
