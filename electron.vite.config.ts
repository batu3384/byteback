import { defineConfig, externalizeDepsPlugin } from 'electron-vite'
import react from '@vitejs/plugin-react'
import type { Plugin } from 'vite'

// CSP hardening (security assessment Fix 3): src/renderer/index.html carries a
// strict meta CSP for production/file:// loads. In dev (`electron-vite dev`)
// the same meta would break HMR: @vitejs/plugin-react injects an inline
// react-refresh preamble (script-src 'self' blocks it) and the Vite client
// opens a ws:// localhost socket (connect-src 'none' blocks it). This dev-only
// transform relaxes the served meta; the build output keeps the strict tag.
function relaxCspForDev(): Plugin {
  const DEV_CSP =
    "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self' ws://localhost:* ws://127.0.0.1:*; object-src 'none'; base-uri 'none'"
  return {
    name: 'byteback-relax-csp-for-dev',
    apply: 'serve',
    transformIndexHtml(html) {
      return html.replace(
        /<meta\s+http-equiv="Content-Security-Policy"[^>]*>/i,
        `<meta http-equiv="Content-Security-Policy" content="${DEV_CSP}" />`,
      )
    },
  }
}

export default defineConfig({
  main: {
    plugins: [externalizeDepsPlugin()]
  },
  preload: {
    plugins: [externalizeDepsPlugin()]
  },
  renderer: {
    plugins: [react(), relaxCspForDev()]
  }
})
