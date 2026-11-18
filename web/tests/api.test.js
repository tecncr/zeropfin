import { describe, expect, it, vi } from 'vitest'
import { ApiError, createApi } from '../src/lib/api.js'

function res(status, body) {
  return { ok: status >= 200 && status < 300, status, text: async () => (body == null ? '' : JSON.stringify(body)) }
}

describe('api client', () => {
  it('sends same-origin GETs without CSRF', async () => {
    const f = vi.fn(async () => res(200, { ok: 1 }))
    const api = createApi({ fetchImpl: f, getCsrf: () => 'c'.repeat(32) })
    expect(await api.get('/status')).toEqual({ ok: 1 })
    const [url, init] = f.mock.calls[0]
    expect(url).toBe('/api/v1/status')
    expect(init.credentials).toBe('same-origin')
    expect(init.headers['X-CSRF-Token']).toBeUndefined()
  })
  it('sends JSON POSTs with the CSRF header', async () => {
    const f = vi.fn(async () => res(202, { state: 'paused' }))
    const api = createApi({ fetchImpl: f, getCsrf: () => 'c'.repeat(32) })
    await api.post('/player/seek', { positionMs: 1000 })
    const [, init] = f.mock.calls[0]
    expect(init.method).toBe('POST')
    expect(init.headers['Content-Type']).toBe('application/json')
    expect(init.headers['X-CSRF-Token']).toBe('c'.repeat(32))
    expect(JSON.parse(init.body)).toEqual({ positionMs: 1000 })
  })
  it('maps error envelopes and flags unauthenticated sessions', async () => {
    const onUnauthorized = vi.fn()
    const api = createApi({
      fetchImpl: async () => res(401, { error: { code: 'unauthenticated', message: 'Pair first', retryable: false } }),
      onUnauthorized,
    })
    const e = await api.get('/views').catch((x) => x)
    expect(e).toBeInstanceOf(ApiError)
    expect(e.status).toBe(401)
    expect(e.code).toBe('unauthenticated')
    expect(onUnauthorized).toHaveBeenCalledOnce()
  })
  it('carries playback details for incompatible media', async () => {
    const api = createApi({
      fetchImpl: async () => res(422, { error: { code: 'incompatible', message: 'HEVC', retryable: false,
        playback: { playable: false, reasons: ['HEVC'] } } }),
    })
    const e = await api.post('/player/play', { itemId: 'x' }).catch((x) => x)
    expect(e.code).toBe('incompatible')
    expect(e.details.reasons).toEqual(['HEVC'])
  })
  it('reports network failures as retryable', async () => {
    const api = createApi({ fetchImpl: async () => { throw new TypeError('Failed to fetch') } })
    const e = await api.get('/player').catch((x) => x)
    expect(e.code).toBe('network')
    expect(e.retryable).toBe(true)
  })
})
