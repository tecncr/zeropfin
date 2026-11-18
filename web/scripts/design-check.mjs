// Static design/packaging QA over the production bundle (web/dist):
//  - only static HTML/CSS/JS/font/image/text files
//  - Inter is bundled locally (woff2 + OFL license), no font CDNs
//  - no remote URLs (CDNs, analytics, Google Fonts) anywhere
//  - no gradients of any kind (CSS functions, SVG gradient elements)
//  - every color in the CSS is neutral gray, black/white or Tailwind blue
//  - no tooltips (title attributes) and no stock "explain the obvious" copy
//  - no inline scripts or inline style attributes (strict CSP)
import { readdirSync, readFileSync, statSync } from 'node:fs'
import { extname, join, relative } from 'node:path'

const dist = new URL('../dist/', import.meta.url).pathname
const failures = []
const fail = (m) => failures.push(m)

function walk(dir) {
  return readdirSync(dir).flatMap((n) => {
    const p = join(dir, n)
    return statSync(p).isDirectory() ? walk(p) : [p]
  })
}
const files = walk(dist)
const allowedExt = new Set(['.html', '.js', '.css', '.woff2', '.svg', '.png', '.txt', '.gz', '.json'])
for (const f of files) {
  if (!allowedExt.has(extname(f))) fail(`unexpected file type in bundle: ${relative(dist, f)}`)
  if (f.endsWith('.map')) fail(`source map shipped: ${relative(dist, f)}`)
}

const text = (f) => readFileSync(f, 'utf8')
const textFiles = files.filter((f) => /\.(html|js|css|svg)$/.test(f))
const html = text(join(dist, 'index.html'))
const css = files.filter((f) => f.endsWith('.css')).map(text).join('\n')

// Fonts
const woff2 = files.filter((f) => f.endsWith('.woff2'))
if (!woff2.some((f) => /inter/i.test(f))) fail('Inter woff2 not bundled')
if (!/font-family:\s*"?Inter"?/i.test(css)) fail('no @font-face for Inter in CSS')
if (!/--font-sans:\s*"?Inter/i.test(css) && !/font-family:\s*var\(--font-sans/.test(css)) fail('Inter is not the UI font')
if (!/font-display:\s*swap/.test(css)) fail('font-display: swap missing')
if (!files.some((f) => f.endsWith('licenses/Inter-OFL.txt'))) fail('Inter OFL license not shipped')

// Remote loads: anything the browser would fetch from another origin. (License
// banners and error-message links inside comments/strings are not fetches; the
// strict CSP blocks remote loads at runtime regardless.)
const remoteLoad = [
  /url\(\s*["']?(https?:)?\/\//i,
  /\s(src|href|action|poster)\s*=\s*["']?(https?:)?\/\//i,
  /@import\s+(url\()?["']?(https?:)?\/\//i,
  /\bimport\s*\(\s*["'`]https?:/i,
  /\bfetch\s*\(\s*["'`]https?:/i,
  /fonts\.(googleapis|gstatic)\.com|unpkg\.com|cdn\.jsdelivr|cdnjs\./i,
]
for (const f of textFiles) {
  const t = text(f)
  for (const re of remoteLoad) {
    const m = t.match(re)
    if (m) fail(`remote load in ${relative(dist, f)}: ${m[0]}`)
  }
}

// Gradients
for (const f of textFiles) {
  const t = text(f)
  if (/(linear|radial|conic)-gradient\s*\(/i.test(t) || /<(linear|radial)Gradient/i.test(t))
    fail(`gradient found in ${relative(dist, f)}`)
}

// Palette: every oklch()/rgb()/hex color must be achromatic or Tailwind blue (hue 245-270).
function rgbOk(r, g, b) {
  const max = Math.max(r, g, b)
  const min = Math.min(r, g, b)
  if (max - min <= 2) return true // gray
  // hue of an sRGB color; Tailwind blue-300..700 lands at ~210-225 degrees
  let hue
  if (max === r) hue = ((g - b) / (max - min)) % 6
  else if (max === g) hue = (b - r) / (max - min) + 2
  else hue = (r - g) / (max - min) + 4
  hue = (hue * 60 + 360) % 360
  return max === b && hue >= 205 && hue <= 235
}
const colorRe = /oklch\(([^)]+)\)|#[0-9a-f]{3,8}\b|rgba?\([^)]+\)|hsla?\([^)]+\)/gi
for (const m of css.matchAll(colorRe)) {
  const c = m[0].toLowerCase()
  if (c.startsWith('oklch')) {
    const parts = m[1].replace('/', ' ').trim().split(/\s+/)
    if (parts.some((p) => p.includes('var('))) continue
    const chroma = parseFloat(parts[1])
    const hue = parseFloat(parts[2])
    if (chroma > 0.004 && !(hue >= 245 && hue <= 270)) fail(`non-palette color ${c}`)
  } else if (c.startsWith('#')) {
    // Tailwind emits sRGB hex fallbacks for its oklch palette.
    const h = c.slice(1)
    const full = h.length <= 4 ? [...h].map((x) => x + x).join('') : h
    const [r, g, b] = [0, 2, 4].map((i) => parseInt(full.slice(i, i + 2), 16))
    if (!rgbOk(r, g, b)) fail(`non-palette hex color ${c}`)
  } else if (c.startsWith('rgb')) {
    if (/\bfrom\b/.test(c)) continue // @supports feature probe, not a used color
    const [r, g, b] = c.match(/[\d.]+/g).map(Number)
    if (!rgbOk(r, g, b)) fail(`non-palette rgb color ${c}`)
  } else {
    fail(`unexpected color syntax ${c}`)
  }
}

// Tooltips, inline script/style, untrusted HTML sinks, redundant copy
if (/\stitle="/i.test(html.replace(/<title>[^<]*<\/title>/i, ''))) fail('title attribute (tooltip) in HTML')
if (/x-html/.test(html)) fail('x-html used')
if (/<script(?![^>]*\bsrc=)[^>]*>/i.test(html)) fail('inline <script> in HTML')
if (/\sstyle="/i.test(html)) fail('inline style attribute in HTML (blocked by CSP)')
if (/\son[a-z]+="/i.test(html)) fail('inline event handler attribute in HTML')
const banned = [/browse your/i, /control your/i, /discover (your|new)/i, /welcome to/i, /manage your/i, /tooltip/i]
for (const re of banned) if (re.test(html)) fail(`redundant explanatory copy matched ${re}`)

if (failures.length) {
  console.error('design check FAILED:\n  ' + failures.join('\n  '))
  process.exit(1)
}
console.log(`design check passed (${files.length} files, ${woff2.length} Inter font files)`)
