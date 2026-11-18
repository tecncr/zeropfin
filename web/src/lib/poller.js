// Player-state polling. The daemon is the source of truth; the browser only
// mirrors it. Fast while something is active and the tab is visible, slow when
// idle, paused while hidden, exponential backoff when the receiver is
// unreachable, and an immediate refresh when Safari brings the tab back.

export function nextDelay({ visible, active, failures }) {
  if (!visible) return null
  if (failures > 0) return Math.min(30000, 1000 * 2 ** Math.min(failures, 5))
  return active ? 1000 : 5000
}

export function isActive(player) {
  return Boolean(player) && ['loading', 'playing', 'paused'].includes(player.state)
}

export function createPoller({ fetchState, onState, onError, doc = globalThis.document, win = globalThis }) {
  let timer = null
  let failures = 0
  let active = false
  let running = false
  let inflight = false

  const visible = () => !doc || doc.visibilityState !== 'hidden'

  function schedule() {
    clearTimeout(timer)
    timer = null
    if (!running) return
    const d = nextDelay({ visible: visible(), active, failures })
    if (d !== null) timer = win.setTimeout(tick, d)
  }

  async function tick() {
    if (inflight) return
    inflight = true
    try {
      const s = await fetchState()
      failures = 0
      active = isActive(s)
      onState(s)
    } catch (e) {
      failures++
      onError(e, failures)
    } finally {
      inflight = false
      schedule()
    }
  }

  function onVisibility() {
    if (visible()) tick()
    else schedule()
  }

  return {
    start() {
      if (running) return
      running = true
      doc && doc.addEventListener('visibilitychange', onVisibility)
      win.addEventListener && win.addEventListener('pageshow', onVisibility)
      win.addEventListener && win.addEventListener('online', onVisibility)
      tick()
    },
    stop() {
      running = false
      clearTimeout(timer)
      doc && doc.removeEventListener('visibilitychange', onVisibility)
    },
    /** Refresh now (after a command) and treat the player as active. */
    poke() {
      active = true
      tick()
    },
  }
}
