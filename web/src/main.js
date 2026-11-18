import Alpine from '@alpinejs/csp'
import './styles.css'
import { ApiError, createApi } from './lib/api.js'
import { episodeCode, formatClock, formatPairCode, formatRuntime, fraction, imageSrc, itemMeta } from './lib/format.js'
import { createPoller, isActive } from './lib/poller.js'
import { href, parseRoute } from './lib/router.js'

const store = {
  boot: 'loading', // loading | ready
  authed: false,
  csrf: null,
  pairingOnDisplay: true,
  status: null,
  player: null,
  online: true,
  route: parseRoute(location.hash),
  views: [],
  notice: null,
}

const api = createApi({
  getCsrf: () => Alpine.store('app').csrf,
  onUnauthorized: () => {
    const s = Alpine.store('app')
    s.authed = false
    s.csrf = null
  },
})

let noticeTimer = null
function notify(text, kind = 'info') {
  const s = Alpine.store('app')
  s.notice = { text, kind }
  clearTimeout(noticeTimer)
  noticeTimer = setTimeout(() => (Alpine.store('app').notice = null), kind === 'error' ? 7000 : 3500)
}

const poller = createPoller({
  fetchState: () => api.get('/player'),
  onState: (p) => {
    const s = Alpine.store('app')
    s.player = p
    s.online = true
  },
  onError: (e, failures) => {
    if (e instanceof ApiError && e.status === 401) return
    if (failures >= 2) Alpine.store('app').online = false
  },
})

async function loadAfterAuth() {
  const s = Alpine.store('app')
  try {
    s.status = await api.get('/status')
    s.player = s.status.player
    if (s.status.jellyfin.configured) {
      const v = await api.get('/views')
      s.views = v.items
    }
  } catch (e) {
    if (!(e instanceof ApiError) || e.status !== 401) notify(e.message, 'error')
  }
  poller.start()
}

async function boot() {
  const s = Alpine.store('app')
  try {
    const sess = await api.get('/session')
    s.authed = sess.authenticated
    s.csrf = sess.csrfToken || null
    s.pairingOnDisplay = sess.pairingOnDisplay
  } catch {
    s.online = false
  }
  s.boot = 'ready'
  if (s.authed) loadAfterAuth()
}

// ---- shared helpers mixed into components (template-callable) ----------------

const common = {
  img(ref, width) {
    return imageSrc(ref, width, globalThis.devicePixelRatio || 1)
  },
  meta(item) {
    return itemMeta(item)
  },
  clock(ms) {
    return formatClock(ms)
  },
  runtime(ms) {
    return formatRuntime(ms)
  },
  epCode(item) {
    return episodeCode(item)
  },
  itemHref(item) {
    return href('item', { id: item.id })
  },
  libraryHref(view) {
    return href('library', { id: view.id })
  },
  progressPct(item) {
    if (!item) return 0
    if (Number.isFinite(item.playedPct)) return Math.round(item.playedPct)
    return Math.round(fraction(item.resumeMs, item.runtimeMs) * 100)
  },
  hideImage(e) {
    e.target.classList.add('invisible')
  },
}

const playerActions = {
  busy: false,
  async play(itemId, resume = true) {
    if (this.busy) return
    this.busy = true
    try {
      await api.post('/player/play', { itemId, resume }, { timeoutMs: 45000 })
      poller.poke()
      location.hash = href('now')
    } catch (e) {
      notify(e.message, 'error')
    } finally {
      this.busy = false
    }
  },
  async command(path, body) {
    try {
      const p = await api.post(path, body)
      Alpine.store('app').player = p
      poller.poke()
    } catch (e) {
      notify(e.message, 'error')
    }
  },
  togglePause() {
    const p = Alpine.store('app').player
    return this.command(p && p.state === 'paused' ? '/player/resume' : '/player/pause')
  },
  skip(deltaMs) {
    return this.command('/player/seek', { deltaMs })
  },
  stop() {
    return this.command('/player/stop')
  },
}

// ---- components -------------------------------------------------------------------

Alpine.store('app', store)

Alpine.data('shell', () => ({
  init() {
    addEventListener('hashchange', () => {
      Alpine.store('app').route = parseRoute(location.hash)
      window.scrollTo(0, 0)
    })
    boot()
  },
  get routeName() {
    return Alpine.store('app').route.name
  },
}))

Alpine.data('pairing', () => ({
  code: '',
  busy: false,
  error: '',
  info: '',
  onInput(e) {
    this.code = formatPairCode(e.target.value)
    e.target.value = this.code
    this.error = ''
  },
  get complete() {
    return this.code.length === 9
  },
  async submit() {
    if (!this.complete || this.busy) return
    this.busy = true
    this.error = ''
    try {
      const name = /iPhone|iPad/.test(navigator.userAgent) ? 'iPhone or iPad' : 'Browser'
      const r = await api.post('/pairing/complete', { code: this.code, deviceName: name })
      const s = Alpine.store('app')
      s.csrf = r.csrfToken
      s.authed = true
      loadAfterAuth()
    } catch (e) {
      this.error = e.message
    } finally {
      this.busy = false
    }
  },
  async showOnTv() {
    this.error = ''
    try {
      const r = await api.post('/pairing/request', {})
      this.info = r.displayRequested ? 'Code shown on the TV for two minutes.' : 'Code written to the service log.'
    } catch (e) {
      this.error = e.message
    }
  },
}))

Alpine.data('searchBox', () => ({
  q: '',
  timer: null,
  init() {
    this.q = Alpine.store('app').route.query.q || ''
    // The header outlives route changes: keep the box in sync with the URL.
    this.$watch('$store.app.route', (r) => {
      if (r.name !== 'search') this.q = ''
      else if (document.activeElement !== this.$el.querySelector('input')) this.q = r.query.q || ''
    })
  },
  onInput(e) {
    this.q = e.target.value
    clearTimeout(this.timer)
    this.timer = setTimeout(() => this.go(), 300)
  },
  go() {
    const q = this.q.trim()
    location.hash = href('search', {}, q ? { q } : undefined)
  },
}))

Alpine.data('home', () => ({
  ...common,
  loading: true,
  error: '',
  resume: [],
  nextUp: [],
  latest: [],
  async init() {
    const s = Alpine.store('app')
    if (s.status && !s.status.jellyfin.configured) {
      this.loading = false
      return
    }
    try {
      const [resume, nextUp, views] = await Promise.all([api.get('/resume'), api.get('/nextup'), api.get('/views')])
      this.resume = resume.items
      this.nextUp = nextUp.items.filter((n) => !resume.items.some((r) => r.id === n.id))
      s.views = views.items
      this.loading = false
      const rows = await Promise.all(
        views.items.map((v) =>
          api
            .get(`/latest?parentId=${v.id}`)
            .then((r) => ({ view: v, items: r.items }))
            .catch(() => ({ view: v, items: [] })),
        ),
      )
      this.latest = rows.filter((r) => r.items.length)
    } catch (e) {
      this.error = e.message
      this.loading = false
    }
  },
  get empty() {
    return !this.loading && !this.error && !this.resume.length && !this.nextUp.length && !this.latest.length
  },
}))

const SORTS = [
  { id: 'name', label: 'Name' },
  { id: 'added', label: 'Date added' },
  { id: 'released', label: 'Release date' },
  { id: 'rating', label: 'Rating' },
]

function typesFor(view) {
  if (!view) return ''
  if (view.collectionType === 'movies') return 'Movie'
  if (view.collectionType === 'tvshows') return 'Series'
  if (view.collectionType === 'boxsets') return 'BoxSet'
  return ''
}

Alpine.data('library', () => ({
  ...common,
  sorts: SORTS,
  items: [],
  total: null,
  loading: false,
  error: '',
  sort: 'name',
  unplayed: false,
  observer: null,
  get view() {
    const id = Alpine.store('app').route.params.id
    return Alpine.store('app').views.find((v) => v.id === id) || null
  },
  get title() {
    return this.view ? this.view.name : 'Library'
  },
  get done() {
    return this.total !== null && this.items.length >= this.total
  },
  init() {
    this.load(true)
    this.$nextTick(() => {
      this.observer = new IntersectionObserver((entries) => {
        if (entries.some((e) => e.isIntersecting)) this.load(false)
      }, { rootMargin: '600px' })
      if (this.$refs.sentinel) this.observer.observe(this.$refs.sentinel)
    })
  },
  destroy() {
    this.observer && this.observer.disconnect()
  },
  setSort(e) {
    this.sort = e.target.value
    this.load(true)
  },
  toggleUnplayed() {
    this.unplayed = !this.unplayed
    this.load(true)
  },
  async load(reset) {
    if (this.loading || (!reset && this.done)) return
    if (reset) {
      this.items = []
      this.total = null
    }
    this.loading = true
    this.error = ''
    const q = new URLSearchParams({
      parentId: Alpine.store('app').route.params.id,
      start: String(this.items.length),
      limit: '36',
      sort: this.sort,
    })
    const types = typesFor(this.view)
    if (types) q.set('types', types)
    if (this.unplayed) q.set('filter', 'unplayed')
    try {
      const r = await api.get(`/items?${q}`)
      this.items = this.items.concat(r.items)
      this.total = Number.isFinite(r.total) ? r.total : this.items.length
    } catch (e) {
      this.error = e.message
    } finally {
      this.loading = false
    }
  },
}))

Alpine.data('search', () => ({
  ...common,
  items: [],
  loading: false,
  error: '',
  searched: false,
  get q() {
    return (Alpine.store('app').route.query.q || '').trim()
  },
  async init() {
    if (!this.q) {
      this.$nextTick(() => document.getElementById('search-input')?.focus())
      return
    }
    this.loading = true
    try {
      const r = await api.get(`/items?${new URLSearchParams({ search: this.q, limit: '48', types: 'Movie,Series,Episode' })}`)
      this.items = r.items
    } catch (e) {
      this.error = e.message
    } finally {
      this.loading = false
      this.searched = true
    }
  },
}))

Alpine.data('item', () => ({
  ...common,
  ...playerActions,
  item: null,
  loading: true,
  error: '',
  seasons: [],
  seasonId: null,
  episodes: [],
  episodesLoading: false,
  async init() {
    const id = Alpine.store('app').route.params.id
    try {
      this.item = await api.get(`/items/${id}`)
      if (this.item.type === 'Series') await this.loadSeasons()
    } catch (e) {
      this.error = e.message
    } finally {
      this.loading = false
    }
  },
  get isSeries() {
    return this.item && this.item.type === 'Series'
  },
  get playback() {
    return this.item && this.item.playback
  },
  get canPlay() {
    return Boolean(this.playback && this.playback.playable)
  },
  get facts() {
    const it = this.item
    if (!it) return ''
    const parts = []
    if (it.type === 'Episode' && this.epCode(it)) parts.push(this.epCode(it))
    if (it.year) parts.push(String(it.year))
    const rt = formatRuntime(it.runtimeMs)
    if (rt) parts.push(rt)
    if (it.officialRating) parts.push(it.officialRating)
    if (Number.isFinite(it.communityRating)) parts.push(`★ ${it.communityRating}`)
    return parts.join(' · ')
  },
  get mediaLine() {
    const m = this.playback && this.playback.media
    if (!m || !m.videoCodec) return ''
    const parts = []
    if (m.height) parts.push(`${m.height}p`)
    parts.push(['H.264', m.videoProfile].filter(Boolean).join(' '))
    if (m.audioCodec) parts.push(m.audioCodec.toUpperCase() + (m.audioChannels ? ` ${m.audioChannels}ch` : ''))
    if (m.container) parts.push(m.container.toUpperCase())
    return parts.join(' · ')
  },
  get genres() {
    return this.item && this.item.genres ? this.item.genres.join(' · ') : ''
  },
  get hasResume() {
    return Boolean(this.item && this.item.resumeMs > 0)
  },
  async loadSeasons() {
    const r = await api.get(`/items/${this.item.id}/seasons`)
    this.seasons = r.items
    const firstUnplayed = this.seasons.find((s) => !s.played && (s.unplayed ?? 1) > 0)
    if (this.seasons.length) this.selectSeason((firstUnplayed || this.seasons[0]).id)
  },
  async selectSeason(id) {
    this.seasonId = id
    this.episodesLoading = true
    try {
      const r = await api.get(`/items/${this.item.id}/episodes?seasonId=${id}&limit=100`)
      if (this.seasonId === id) this.episodes = r.items
    } catch (e) {
      notify(e.message, 'error')
    } finally {
      this.episodesLoading = false
    }
  },
  isSeason(s) {
    return s.id === this.seasonId
  },
}))

Alpine.data('nowPlaying', () => ({
  ...common,
  ...playerActions,
  dragging: false,
  dragValue: 0,
  nav: { prev: null, next: null, forId: null },
  get p() {
    return Alpine.store('app').player
  },
  get hasItem() {
    return Boolean(this.p && this.p.item)
  },
  get active() {
    return isActive(this.p)
  },
  get duration() {
    return (this.p && (this.p.durationMs || (this.p.item && this.p.item.runtimeMs))) || 0
  },
  get position() {
    return this.dragging ? this.dragValue : (this.p ? this.p.positionMs : 0)
  },
  get pct() {
    return Math.round(fraction(this.position, this.duration) * 1000) / 10
  },
  get estimated() {
    return this.p && this.p.positionSource === 'estimated'
  },
  get canControl() {
    return this.p && (this.p.state === 'playing' || this.p.state === 'paused')
  },
  get paused() {
    return this.p && this.p.state === 'paused'
  },
  get stateLabel() {
    const st = this.p && this.p.state
    return { loading: 'Starting…', paused: 'Paused', ended: 'Finished', stopped: 'Stopped', error: 'Playback stopped' }[st] || ''
  },
  get remaining() {
    return this.duration ? '-' + formatClock(Math.max(0, this.duration - this.position)) : ''
  },
  get seekLabel() {
    return `${formatClock(this.position)} of ${formatClock(this.duration)}`
  },
  init() {
    this.$watch('p', () => this.loadNav())
    this.loadNav()
  },
  async loadNav() {
    const it = this.p && this.p.item
    if (!it || it.type !== 'Episode' || this.nav.forId === it.id) return
    this.nav = { prev: null, next: null, forId: it.id }
    try {
      const d = await api.get(`/items/${it.id}`)
      if (!d.seriesId || !d.seasonId) return
      const eps = (await api.get(`/items/${d.seriesId}/episodes?seasonId=${d.seasonId}&limit=100`)).items
      const i = eps.findIndex((e) => e.id === it.id)
      if (this.nav.forId !== it.id || i < 0) return
      this.nav = { prev: eps[i - 1] || null, next: eps[i + 1] || null, forId: it.id }
    } catch {
      /* navigation is optional */
    }
  },
  onSeekInput(e) {
    this.dragging = true
    this.dragValue = Number(e.target.value)
  },
  async onSeekChange(e) {
    this.dragValue = Number(e.target.value)
    await this.command('/player/seek', { positionMs: Math.round(this.dragValue) })
    this.dragging = false
  },
  playNav(ep) {
    if (ep) this.play(ep.id, false)
  },
}))

Alpine.data('miniPlayer', () => ({
  ...common,
  ...playerActions,
  get p() {
    return Alpine.store('app').player
  },
  get visible() {
    const s = Alpine.store('app')
    return s.authed && s.route.name !== 'now' && this.p && this.p.item && isActive(this.p)
  },
  get pct() {
    const p = this.p
    if (!p) return 0
    return Math.round(fraction(p.positionMs, p.durationMs || (p.item && p.item.runtimeMs)) * 1000) / 10
  },
  get paused() {
    return this.p && this.p.state === 'paused'
  },
}))

window.Alpine = Alpine
Alpine.start()
