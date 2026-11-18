// Same-origin JSON client for the Zeropfin daemon (/api/v1). The browser never
// talks to Jellyfin and never sees its token; authentication is an HttpOnly
// cookie plus a CSRF header on state-changing requests.

export class ApiError extends Error {
  constructor(status, code, message, retryable = false, details = null) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.code = code
    this.retryable = retryable
    this.details = details
  }
}

export function createApi({ fetchImpl = (...a) => fetch(...a), getCsrf = () => null, onUnauthorized = () => {} } = {}) {
  async function request(method, path, body, { timeoutMs = 15000 } = {}) {
    const ctrl = new AbortController()
    const timer = setTimeout(() => ctrl.abort(), timeoutMs)
    const headers = { Accept: 'application/json' }
    const init = { method, headers, credentials: 'same-origin', cache: 'no-store', signal: ctrl.signal }
    if (method === 'POST') {
      headers['Content-Type'] = 'application/json'
      const csrf = getCsrf()
      if (csrf) headers['X-CSRF-Token'] = csrf
      init.body = JSON.stringify(body || {})
    }
    let res
    try {
      res = await fetchImpl(`/api/v1${path}`, init)
    } catch (e) {
      throw new ApiError(0, e && e.name === 'AbortError' ? 'timeout' : 'network', 'Cannot reach the receiver.', true)
    } finally {
      clearTimeout(timer)
    }
    let data = null
    const text = await res.text()
    if (text) {
      try {
        data = JSON.parse(text)
      } catch {
        data = null
      }
    }
    if (!res.ok) {
      const e = (data && data.error) || {}
      if (res.status === 401 && e.code === 'unauthenticated') onUnauthorized()
      throw new ApiError(res.status, e.code || 'http_' + res.status, e.message || `Request failed (${res.status}).`,
        Boolean(e.retryable), e.playback || null)
    }
    return data
  }
  return {
    get: (path, opts) => request('GET', path, null, opts),
    post: (path, body, opts) => request('POST', path, body, opts),
  }
}
