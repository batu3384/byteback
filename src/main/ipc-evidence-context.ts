import { getEngine } from './native-bridge'
import { isDestOnEvidence } from '../shared/recover-dest-guard'
import { ERR_DEST_ON_SOURCE, ERR_RAID_STATE_UNREAD } from './recover-dest-validator'

let lastEvidenceDisks: number[] = []

export function setLastEvidenceDisks(disks: number[]): void {
  lastEvidenceDisks = disks
}

export function getLastEvidenceDisks(): readonly number[] {
  return lastEvidenceDisks
}

export function lastEvidenceDisksIncludes(driveIndex: number): boolean {
  return lastEvidenceDisks.includes(driveIndex)
}

/** Block recover/wipe destinations that sit on evidence media. */
export function destOnEvidenceError(destDir: string, driveIndex: number, scanId?: number, extraDisks?: number[]): string | null {
  const engine = getEngine()
  let raid: { active: boolean; memberDriveIndices?: number[] }
  try {
    raid = engine.getRaidState()
  } catch {
    return ERR_RAID_STATE_UNREAD
  }
  const members = [...(raid.active ? (raid.memberDriveIndices ?? []) : [])]
  if (typeof scanId === 'number' && scanId > 0) {
    try {
      const st = engine.getScanState(scanId)
      const disks = Array.isArray(st?.evidenceDiskIndices) ? st.evidenceDiskIndices : []
      const cleaned = disks.filter((n) => Number.isInteger(n) && n >= 0)
      members.push(...(cleaned.length > 0 ? cleaned : lastEvidenceDisks))
    } catch {
      members.push(...lastEvidenceDisks)
    }
  } else {
    members.push(...lastEvidenceDisks)
  }
  if (extraDisks) {
    members.push(...extraDisks.filter((n) => Number.isInteger(n) && n >= 0))
  }
  if (isDestOnEvidence(destDir, driveIndex, members, (letter) => engine.resolveVolume(letter))) {
    return ERR_DEST_ON_SOURCE
  }
  return null
}
