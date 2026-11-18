import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import tailwindcss from '@tailwindcss/vite'
import { defineConfig } from 'vite'

const interDir = fileURLToPath(new URL('./node_modules/@fontsource-variable/inter/files', import.meta.url))
const interLicense = fileURLToPath(new URL('./node_modules/@fontsource-variable/inter/LICENSE', import.meta.url))

// Ship the Inter (SIL OFL 1.1) license next to the bundled font files.
function licenses() {
  return {
    name: 'zeropfin-licenses',
    generateBundle() {
      this.emitFile({ type: 'asset', fileName: 'licenses/Inter-OFL.txt', source: readFileSync(interLicense) })
    },
  }
}

// `pnpm dev` talks to a locally running daemon (see scripts/dev-stack.py).
const daemon = process.env.ZEROPFIN_DEV_DAEMON || 'http://127.0.0.1:8080'

export default defineConfig({
  base: '/',
  plugins: [tailwindcss(), licenses()],
  resolve: { alias: { '@inter': interDir } },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    assetsDir: 'assets',
    target: 'es2020', // iOS Safari 14+
    sourcemap: false,
    assetsInlineLimit: 0, // never inline fonts/images as data: URIs (CSP font-src 'self')
    modulePreload: { polyfill: false }, // no inline scripts
    cssCodeSplit: false,
  },
  server: {
    proxy: {
      '/api': {
        target: daemon,
        changeOrigin: true,
        configure: (proxy) => {
          // The daemon enforces Origin == Host on POST; present the daemon's origin in dev.
          proxy.on('proxyReq', (req) => {
            if (req.getHeader('origin')) req.setHeader('origin', daemon)
          })
        },
      },
      '/healthz': { target: daemon, changeOrigin: true },
    },
  },
})
