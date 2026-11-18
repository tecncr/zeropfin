// Browser smoke + visual QA against the full dev stack (fake Jellyfin + real
// daemon + mock ZeroPlay). Runs on the developer host/CI only, never on the Pi.
//
//   CHROMIUM=/usr/bin/chromium-browser ZEROPFIN_BUILD=../build node tests/e2e.mjs
//
// Engine note: this drives Chromium with an iPhone-sized viewport and touch.
// It is NOT WebKit; real iPhone Safari behaviour is a hardware checklist item.
import { spawn } from 'node:child_process'
import { mkdirSync } from 'node:fs'
import { createServer } from 'node:net'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { chromium } from 'playwright-core'

const here = dirname(fileURLToPath(import.meta.url))
const root = resolve(here, '..', '..')
const build = resolve(process.env.ZEROPFIN_BUILD || join(root, 'build'))
const shots = resolve(process.env.SCREENSHOT_DIR || join(root, 'docs', 'screenshots'))
mkdirSync(shots, { recursive: true })

let failures = 0
let checks = 0
function check(cond, what) {
  checks++
  if (!cond) {
    failures++
    console.error(`  FAIL: ${what}`)
  }
  return cond
}

function freePort() {
  return new Promise((res) => {
    const s = createServer().listen(0, '127.0.0.1', () => {
      const p = s.address().port
      s.close(() => res(p))
    })
  })
}

async function startStack() {
  const port = await freePort()
  const child = spawn('python3', ['-I', join(root, 'scripts', 'dev-stack.py'), '--build', build, '--port', String(port),
    '--mock-flags', 'fetch bytes-per-ms=10'], { stdio: ['ignore', 'pipe', 'pipe'] })
  let log = ''
  child.stderr.on('data', (d) => (log += d))
  const info = await new Promise((res, rej) => {
    let buf = ''
    child.stdout.on('data', (d) => {
      buf += d
      const line = buf.split('\n').find((l) => l.startsWith('{'))
      if (line) res(JSON.parse(line))
    })
    child.on('exit', (c) => rej(new Error(`dev stack exited ${c}\n${log}`)))
    setTimeout(() => rej(new Error('dev stack timeout\n' + log)), 20000)
  })
  return { child, info, port }
}

// Runtime design QA executed inside the page.
async function designAudit(page, label) {
  const r = await page.evaluate(() => {
    const bad = []
    const parse = (c) => {
      const m = c.match(/rgba?\(([^)]+)\)/)
      if (!m) return null
      const [r, g, b, a = 1] = m[1].split(/[ ,/]+/).filter(Boolean).map(Number)
      return { r, g, b, a }
    }
    const okColor = (c) => {
      const v = parse(c)
      if (!v || v.a === 0) return true
      const max = Math.max(v.r, v.g, v.b)
      const min = Math.min(v.r, v.g, v.b)
      if (max - min <= 3) return true
      let h
      if (max === v.r) h = ((v.g - v.b) / (max - min)) % 6
      else if (max === v.g) h = (v.b - v.r) / (max - min) + 2
      else h = (v.r - v.g) / (max - min) + 4
      h = (h * 60 + 360) % 360
      return max === v.b && h >= 205 && h <= 235
    }
    for (const el of document.querySelectorAll('body *')) {
      if (el.closest('svg.hidden') || el.tagName === 'IMG') continue
      const cs = getComputedStyle(el)
      if (cs.display === 'none') continue
      if (/gradient/i.test(cs.backgroundImage)) bad.push(`gradient on ${el.tagName}.${el.className}`)
      for (const prop of ['color', 'backgroundColor', 'borderTopColor', 'outlineColor', 'fill', 'stroke']) {
        const v = cs[prop]
        if (v && v !== 'none' && !v.startsWith('url') && !okColor(v)) bad.push(`${prop} ${v} on ${el.tagName}`)
      }
      if (el.hasAttribute('title')) bad.push(`title attribute on ${el.tagName}`)
    }
    const font = getComputedStyle(document.body).fontFamily
    return { bad: [...new Set(bad)].slice(0, 10), font, interLoaded: document.fonts.check('16px Inter') }
  })
  check(r.bad.length === 0, `${label}: monochrome+blue palette, no gradients/tooltips (${r.bad.join('; ')})`)
  check(/^"?Inter"?/.test(r.font), `${label}: Inter is the body font (${r.font})`)
  check(r.interLoaded, `${label}: self-hosted Inter loaded`)
}

async function run() {
  const { child, info } = await startStack()
  const browser = await chromium.launch({ executablePath: process.env.CHROMIUM || '/usr/bin/chromium-browser',
    args: ['--no-sandbox'] })
  const cspViolations = []
  const remoteRequests = []
  const watch = (page) => {
    page.on('console', (m) => {
      if (m.type() === 'error' && /Content Security Policy|Refused to/i.test(m.text())) cspViolations.push(m.text())
    })
    page.on('pageerror', (e) => cspViolations.push('pageerror: ' + e.message))
    page.on('request', (req) => {
      if (!req.url().startsWith(info.url.replace(/\/$/, ''))) remoteRequests.push(req.url())
    })
  }
  try {
    // ---------------- iPhone-sized ----------------
    console.log('- iPhone viewport (Chromium engine)')
    const phone = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 3,
      isMobile: true, hasTouch: true,
      userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1' })
    const page = await phone.newPage()
    watch(page)
    await page.goto(info.url)
    await page.getByLabel('Pairing code').waitFor()
    await page.screenshot({ path: join(shots, 'iphone-pairing.png') })
    await designAudit(page, 'pairing')
    await page.getByLabel('Pairing code').fill('wxyz')
    check((await page.getByLabel('Pairing code').inputValue()) === 'WXYZ', 'code input uppercases')
    await page.getByLabel('Pairing code').fill(info.pairingCode.toLowerCase().replace('-', ''))
    check((await page.getByLabel('Pairing code').inputValue()) === info.pairingCode, 'code input formats XXXX-XXXX')
    await page.getByRole('button', { name: 'Pair' }).click()
    await page.getByRole('heading', { name: 'Continue watching' }).waitFor()
    await page.waitForFunction(() => [...document.images].filter((i) => i.complete && i.naturalWidth > 0).length >= 4)
    await page.screenshot({ path: join(shots, 'iphone-home.png') })
    await designAudit(page, 'home')
    check(await page.getByRole('link', { name: 'See all' }).first().isVisible(), 'library rows with See all')

    await page.getByRole('link', { name: 'Movies' }).first().click()
    await page.getByRole('heading', { name: 'Movies' }).waitFor()
    await page.waitForFunction(() => document.querySelectorAll('main li a').length >= 36)
    await page.mouse.wheel(0, 20000)
    await page.waitForFunction(() => document.querySelectorAll('main li a').length === 41, null, { timeout: 8000 })
      .then(() => check(true, ''), () => check(false, 'infinite scroll loads all 41 movies'))
    await page.evaluate(() => window.scrollTo(0, 0))
    await page.screenshot({ path: join(shots, 'iphone-library.png') })

    await page.getByRole('searchbox', { name: 'Search' }).fill('quiet')
    await page.getByRole('heading', { name: /Results for/ }).waitFor()
    await page.getByRole('link', { name: /Quiet Engine/ }).click()
    await page.getByText('Can’t play on this receiver').waitFor()
    check(await page.getByText(/HEVC is not supported/).isVisible(), 'incompatible reason shown')
    check(!(await page.getByRole('button', { name: /^Play/ }).count()), 'no play button for incompatible media')
    await page.screenshot({ path: join(shots, 'iphone-incompatible.png') })

    await page.goto(info.url + '#/search?q=harbor')
    await page.getByRole('link', { name: /Harbor Lights/ }).click()
    await page.getByRole('heading', { name: 'Harbor Lights' }).waitFor()
    await page.screenshot({ path: join(shots, 'iphone-item.png') })
    await designAudit(page, 'details')
    await page.getByRole('button', { name: 'Play', exact: true }).click()
    await page.getByRole('heading', { name: 'Harbor Lights', level: 2 }).waitFor()
    await page.waitForFunction(() => document.querySelector('[aria-label="Pause"]') && !document.querySelector('[aria-label="Pause"]').disabled)
    await page.waitForTimeout(1500)
    await page.screenshot({ path: join(shots, 'iphone-now-playing.png') })
    await designAudit(page, 'now playing')
    await page.getByRole('button', { name: 'Pause' }).first().click()
    await page.getByText('Paused', { exact: true }).waitFor()
    const before = await page.locator('input[type=range]').inputValue()
    await page.getByRole('button', { name: 'Forward 30 seconds' }).click()
    await page.waitForFunction((b) => Number(document.querySelector('input[type=range]').value) >= Number(b) + 29000, before)
      .then(() => check(true, ''), () => check(false, '+30 s seek reflected'))

    // Safari tab suspension: hidden -> no polling; visible -> immediate refresh
    let polls = 0
    page.on('request', (r) => r.url().endsWith('/api/v1/player') && polls++)
    await page.evaluate(() => {
      Object.defineProperty(document, 'visibilityState', { value: 'hidden', configurable: true })
      document.dispatchEvent(new Event('visibilitychange'))
    })
    polls = 0
    await page.waitForTimeout(3000)
    check(polls === 0, `no polling while hidden (${polls})`)
    await page.evaluate(() => {
      Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true })
      document.dispatchEvent(new Event('visibilitychange'))
    })
    await page.waitForTimeout(400)
    check(polls >= 1, 'immediate refresh when visible again')
    await page.reload()
    await page.getByText('Paused', { exact: true }).waitFor()
    check(true, 'state restored from daemon after reload')

    await page.getByRole('button', { name: 'Play' }).first().click()
    await page.goto(info.url + '#/')
    await page.getByRole('link', { name: 'Open now playing' }).waitFor()
    await page.screenshot({ path: join(shots, 'iphone-home-miniplayer.png') })
    await page.getByRole('link', { name: 'Open now playing' }).click()
    await page.getByRole('button', { name: 'Stop' }).click()
    await page.getByText('Stopped', { exact: true }).waitFor()

    await page.goto(info.url + '#/search?q=lantern')
    await page.getByRole('link', { name: /Lantern Bay/ }).click()
    await page.getByRole('tab', { name: 'Season 1' }).waitFor()
    await page.getByText('Low Tide').waitFor()
    await page.screenshot({ path: join(shots, 'iphone-series.png') })
    await page.getByRole('tab', { name: 'Season 2' }).click()
    await page.getByText('New Keeper').waitFor()
    check(true, 'season switch')

    // ---------------- Desktop ----------------
    console.log('- desktop viewport')
    const desk = await browser.newContext({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: 1,
      storageState: await phone.storageState() })
    const dpage = await desk.newPage()
    watch(dpage)
    await dpage.goto(info.url)
    await dpage.getByRole('heading', { name: 'Continue watching' }).waitFor()
    await dpage.waitForFunction(() => [...document.images].filter((i) => i.complete && i.naturalWidth > 0).length >= 6)
    await dpage.screenshot({ path: join(shots, 'desktop-home.png') })
    await designAudit(dpage, 'desktop home')
    await dpage.goto(info.url + '#/search?q=northern')
    await dpage.getByRole('link', { name: /Northern Line/ }).click()
    await dpage.getByRole('button', { name: /Resume 10:00/ }).waitFor()
    await dpage.screenshot({ path: join(shots, 'desktop-item.png') })
    await dpage.getByRole('button', { name: /Resume 10:00/ }).click()
    await dpage.getByText(/^1:2\d:\d\d$/).first().waitFor({ timeout: 8000 }).catch(() => {})
    await dpage.waitForTimeout(1200)
    await dpage.screenshot({ path: join(shots, 'desktop-now-playing.png') })
    await dpage.keyboard.press('Tab')
    const focusOutline = await dpage.evaluate(() => getComputedStyle(document.activeElement).outlineStyle)
    check(focusOutline !== 'none', 'visible keyboard focus')
    await dpage.getByRole('button', { name: 'Stop' }).click()

    check(cspViolations.length === 0, `no CSP violations or page errors (${cspViolations.join(' | ')})`)
    check(remoteRequests.length === 0, `no cross-origin requests (${remoteRequests.join(', ')})`)
  } finally {
    await browser.close()
    child.kill('SIGTERM')
  }
  console.log(`\n${checks - failures} checks passed, ${failures} failed; screenshots in ${shots}`)
  process.exit(failures ? 1 : 0)
}

run().catch((e) => {
  console.error(e)
  process.exit(1)
})
