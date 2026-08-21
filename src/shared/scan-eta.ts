export type EtaSample = { timestamp: number; work: number }

export type EtaResult = {
  history: EtaSample[]
  speed: number
  etaSeconds: number
  stalled: boolean
}

/** Sliding-window speed from monotonic work only. Backward ticks and budget jumps are dropped. */
export function etaFromMonotonicWindow(
  history: EtaSample[],
  current: number,
  total: number,
  now: number,
  windowMs = 30_000,
  prevEma = 0,
  alpha = 0.15,
  stallMs = 8_000,
): EtaResult {
  const last = history.length > 0 ? history[history.length - 1].work : 0
  let nextHist = history.slice()

  const jumpFloor = Math.max(1, total * 0.02)
  if (current < last) {
    // ignore rewind
  } else if (history.length > 0 && current - last > jumpFloor) {
    nextHist = [{ timestamp: now, work: current }]
  } else if (current > last || history.length === 0) {
    nextHist.push({ timestamp: now, work: current })
  }

  while (nextHist.length > 1 && now - nextHist[0].timestamp > windowMs) {
    nextHist.shift()
  }

  let inst = 0
  if (nextHist.length > 1) {
    const first = nextHist[0]
    const end = nextHist[nextHist.length - 1]
    const dt = (end.timestamp - first.timestamp) / 1000
    if (dt > 0) inst = (end.work - first.work) / dt
  }

  const lastTs = nextHist.length > 0 ? nextHist[nextHist.length - 1].timestamp : now
  const stalled = nextHist.length > 0 && current === nextHist[nextHist.length - 1].work && now - lastTs >= stallMs

  const speed = stalled ? 0 : (prevEma > 0 ? prevEma * (1 - alpha) + inst * alpha : inst)
  let etaSeconds = -1
  if (!stalled && speed > 0 && total > current) {
    etaSeconds = (total - current) / speed
    if (etaSeconds > 36 * 3600) etaSeconds = 36 * 3600
  }
  return { history: nextHist, speed, etaSeconds, stalled }
}

export function formatEtaClock(seconds: number): string {
  if (seconds < 0) return '—'
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = Math.floor(seconds % 60)
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
}

export function scanPhaseLabel(phase?: string): string {
  return phase === 'carve' ? 'Oyma — boş alan imza taraması' : 'Metadata — dosya tablosu'
}

export function scanStepIndex(phase?: string, scanType?: string): { step: number; of: number } {
  if (scanType !== 'deep' && scanType !== 'full_carve') return { step: 1, of: 1 }
  return { step: phase === 'carve' ? 2 : 1, of: 2 }
}
