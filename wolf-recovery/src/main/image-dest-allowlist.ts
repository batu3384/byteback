import { readFileSync, writeFileSync, mkdirSync } from 'fs'
import { dirname } from 'path'

export function loadAllowedImageDest(filePath: string): Set<string> {
  try {
    const raw = readFileSync(filePath, 'utf8')
    const parsed = JSON.parse(raw) as unknown
    if (!Array.isArray(parsed)) return new Set()
    return new Set(parsed.filter((p): p is string => typeof p === 'string' && p.length > 0))
  } catch {
    return new Set()
  }
}

export function saveAllowedImageDest(filePath: string, dests: Iterable<string>): void {
  mkdirSync(dirname(filePath), { recursive: true })
  writeFileSync(filePath, JSON.stringify([...dests]), 'utf8')
}
