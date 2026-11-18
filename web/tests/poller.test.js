import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPoller, isActive, nextDelay } from '../src/lib/poller.js'

describe('poll scheduling', () => {
  it('chooses delays', () => {
    expect(nextDelay({ visible: false, active: true, failures: 0 })).toBeNull()
    expect(nextDelay({ visible: true, active: true, failures: 0 })).toBe(1000)
    expect(nextDelay({ visible: true, active: false, failures: 0 })).toBe(5000)
    expect(nextDelay({ visible: true, active: true, failures: 1 })).toBe(2000)
    expect(nextDelay({ visible: true, active: true, failures: 3 })).toBe(8000)
    expect(nextDelay({ visible: true, active: true, failures: 20 })).toBe(30000)
  })
  it('classifies active states', () => {
    expect(isActive({ state: 'playing' })).toBe(true)
    expect(isActive({ state: 'paused' })).toBe(true)
    expect(isActive({ state: 'ended' })).toBe(false)
    expect(isActive(null)).toBe(false)
  })
})

describe('poller', () => {
  beforeEach(() => vi.useFakeTimers())
  afterEach(() => vi.useRealTimers())

  function fakeDoc() {
    const listeners = {}
    return {
      visibilityState: 'visible',
      addEventListener: (t, f) => (listeners[t] = f),
      removeEventListener: () => {},
      fire: (t) => listeners[t] && listeners[t](),
    }
  }

  it('polls fast when active, stops while hidden and refreshes on return', async () => {
    const doc = fakeDoc()
    const fetchState = vi.fn(async () => ({ state: 'playing' }))
    const p = createPoller({ fetchState, onState: () => {}, onError: () => {}, doc, win: globalThis })
    p.start()
    await vi.advanceTimersByTimeAsync(3100)
    expect(fetchState).toHaveBeenCalledTimes(4)
    doc.visibilityState = 'hidden'
    doc.fire('visibilitychange')
    await vi.advanceTimersByTimeAsync(10000)
    const hiddenCalls = fetchState.mock.calls.length
    await vi.advanceTimersByTimeAsync(60000)
    expect(fetchState.mock.calls.length).toBe(hiddenCalls)
    doc.visibilityState = 'visible'
    doc.fire('visibilitychange')
    await vi.advanceTimersByTimeAsync(10)
    expect(fetchState.mock.calls.length).toBe(hiddenCalls + 1)
    p.stop()
  })

  it('backs off exponentially on errors', async () => {
    const doc = fakeDoc()
    const onError = vi.fn()
    const fetchState = vi.fn(async () => { throw new Error('down') })
    const p = createPoller({ fetchState, onState: () => {}, onError, doc, win: globalThis })
    p.start()
    await vi.advanceTimersByTimeAsync(2000 + 4000 + 8000 + 10)
    expect(fetchState).toHaveBeenCalledTimes(4)
    expect(onError).toHaveBeenLastCalledWith(expect.any(Error), 4)
    p.stop()
  })
})
