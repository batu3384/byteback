export type EtaSample = { timestamp: number; work: number }

/** Sliding-window speed from monotonic work only. Backward ticks are dropped. */
export function etaFromMonotonicWindow(
  history: EtaSample[],
  current: number,
  total: number,
  now: number,
  windowMs = 5000,
  prevEma = 0,
  alpha = 0.3,
): { history: EtaSample[]; speed: number; etaSeconds: number } {
  const last = history.length > 0 ? history[history.length - 1].work : 0
  const nextHist = history.slice()
  if (current >= last) {
    nextHist.push({ timestamp: now, work: current })
  }
  while (nextHist.length > 0 && now - nextHist[0].timestamp > windowMs) {
    nextHist.shift()
  }

  let inst = 0
  if (nextHist.length > 1) {
    const first = nextHist[0]
    const end = nextHist[nextHist.length - 1]
    const dt = (end.timestamp - first.timestamp) / 1000
    if (dt > 0) inst = (end.work - first.work) / dt
  }
  const speed = prevEma > 0 ? prevEma * (1 - alpha) + inst * alpha : inst
  let etaSeconds = -1
  if (speed > 0 && total > current) {
    etaSeconds = (total - current) / speed
  }
  return { history: nextHist, speed, etaSeconds }
}
