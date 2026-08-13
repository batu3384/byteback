/**
 * Shannon entropy (bits/byte) over a 0..255 byte stream.
 *
 * Used by the hex viewer and result analysis to classify a byte region as
 * structured data, random/encrypted/high-entropy content, or sparse/zeroed
 * data. Pure and side-effect free so it can be unit-tested in isolation.
 *
 * @param buffer bytes in the 0..255 range (a `number[]` from the IPC layer or
 *   a `Uint8Array` from elsewhere). Values outside 0..255 are masked.
 * @returns entropy in the range [0, 8], where 0 means a single repeated value
 *   and 8 means a perfectly uniform distribution across all 256 byte values.
 */
export function calculateEntropy(buffer: ArrayLike<number>): number {
  if (!buffer || buffer.length === 0) return 0

  const freq = new Map<number, number>()
  for (let i = 0; i < buffer.length; i++) {
    const b = buffer[i] & 0xff
    freq.set(b, (freq.get(b) ?? 0) + 1)
  }

  let entropy = 0
  const len = buffer.length
  for (const count of freq.values()) {
    const p = count / len
    entropy -= p * Math.log2(p)
  }
  return entropy
}

/**
 * Coarse three-band classification used by the hex viewer's color bar.
 * Thresholds mirror the heuristics in HexEditor.tsx so the UI and any
 * downstream tooling agree on what "low/mid/high" means.
 */
export function classifyEntropy(entropy: number): 'low' | 'mid' | 'high' {
  if (entropy > 7.0) return 'high'
  if (entropy > 4.5) return 'mid'
  return 'low'
}
