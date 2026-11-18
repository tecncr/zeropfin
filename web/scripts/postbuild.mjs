// Post-build: precompress text assets (the Pi serves *.gz directly, no CPU spent
// compressing on the receiver), report first-party JS+CSS gzip size against the
// 200 KiB target, and write dist/build-report.json for CI evidence.
import { readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs'
import { join, relative } from 'node:path'
import { gzipSync } from 'node:zlib'

const dist = new URL('../dist/', import.meta.url).pathname
const TEXT = /\.(js|css|html|svg|txt|json|webmanifest)$/
const BUDGET = 200 * 1024

function walk(dir) {
  return readdirSync(dir).flatMap((n) => {
    const p = join(dir, n)
    return statSync(p).isDirectory() ? walk(p) : [p]
  })
}

const files = walk(dist).filter((p) => !p.endsWith('.gz') && !p.endsWith('build-report.json'))
const report = { files: [], jsCssGzipBytes: 0, budgetBytes: BUDGET }
for (const p of files) {
  const raw = readFileSync(p)
  const rel = relative(dist, p)
  const entry = { file: rel, bytes: raw.length }
  if (TEXT.test(p)) {
    const gz = gzipSync(raw, { level: 9 })
    entry.gzipBytes = gz.length
    if (gz.length < raw.length) writeFileSync(p + '.gz', gz)
    if (/\.(js|css)$/.test(p)) report.jsCssGzipBytes += gz.length
  }
  report.files.push(entry)
}
report.files.sort((a, b) => a.file.localeCompare(b.file))
writeFileSync(join(dist, 'build-report.json'), JSON.stringify(report, null, 2) + '\n')
for (const f of report.files) {
  const gz = f.gzipBytes ? ` (${(f.gzipBytes / 1024).toFixed(1)} KiB gzip)` : ''
  console.log(`  ${f.file.padEnd(48)} ${(f.bytes / 1024).toFixed(1).padStart(7)} KiB${gz}`)
}
const kib = (report.jsCssGzipBytes / 1024).toFixed(1)
console.log(`first-party JS+CSS: ${kib} KiB gzip (target < ${BUDGET / 1024} KiB)`)
if (report.jsCssGzipBytes > BUDGET) {
  console.error('bundle size target exceeded')
  process.exit(1)
}
