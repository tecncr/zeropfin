// Pure presentation helpers (unit-tested in tests/format.test.js).

/** 0:42, 12:05, 1:02:03 */
export function formatClock(ms) {
  if (!Number.isFinite(ms) || ms < 0) ms = 0
  const total = Math.floor(ms / 1000)
  const h = Math.floor(total / 3600)
  const m = Math.floor((total % 3600) / 60)
  const s = total % 60
  const ss = String(s).padStart(2, '0')
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${m}:${ss}`
}

/** "1 h 32 min", "48 min", "" for unknown */
export function formatRuntime(ms) {
  if (!Number.isFinite(ms) || ms <= 0) return ''
  const min = Math.round(ms / 60000)
  if (min < 60) return `${Math.max(min, 1)} min`
  const h = Math.floor(min / 60)
  const rest = min % 60
  return rest ? `${h} h ${rest} min` : `${h} h`
}

/** "S1 · E2" for episodes, "" otherwise */
export function episodeCode(item) {
  if (!item || item.type !== 'Episode') return ''
  const s = Number.isFinite(item.parentIndex) ? `S${item.parentIndex}` : ''
  const e = Number.isFinite(item.index) ? `E${item.index}` : ''
  return [s, e].filter(Boolean).join(' · ')
}

/** Secondary line for cards: year for movies, "S1 · E2" for episodes. */
export function itemMeta(item) {
  if (!item) return ''
  if (item.type === 'Episode') return episodeCode(item)
  if (item.type === 'Series') return item.year ? String(item.year) : ''
  const parts = []
  if (item.year) parts.push(String(item.year))
  const rt = formatRuntime(item.runtimeMs)
  if (rt) parts.push(rt)
  return parts.join(' · ')
}

const WIDTHS = [160, 240, 320, 480, 640, 960, 1280]

/** Same-origin image URL for an image ref {id,type,tag}; width snapped to a small set for cacheability. */
export function imageSrc(ref, cssWidth, dpr = 2) {
  if (!ref || !ref.id || !ref.type) return ''
  const want = Math.max(1, Math.round(cssWidth * Math.min(dpr || 1, 3)))
  const w = WIDTHS.find((x) => x >= want) || WIDTHS[WIDTHS.length - 1]
  const q = new URLSearchParams({ w: String(w) })
  if (ref.tag) q.set('tag', ref.tag)
  return `/api/v1/images/${encodeURIComponent(ref.id)}/${encodeURIComponent(ref.type)}?${q}`
}

/** Progress fraction 0..1 */
export function fraction(pos, dur) {
  if (!Number.isFinite(pos) || !Number.isFinite(dur) || dur <= 0) return 0
  return Math.min(1, Math.max(0, pos / dur))
}

/** Normalize a typed pairing code: uppercase, strip separators, insert the dash. */
export function formatPairCode(input) {
  const raw = String(input || '')
    .toUpperCase()
    .replace(/[^A-Z0-9]/g, '')
    .slice(0, 8)
  return raw.length > 4 ? `${raw.slice(0, 4)}-${raw.slice(4)}` : raw
}
