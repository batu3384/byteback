import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'

// Vitest configuration for the renderer/TypeScript layer.
// Includes src/main utilities that don't require Electron at import time
// (e.g. native-addon-path, image-dest-allowlist); electron-coupled main code
// is covered by e2e.
export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'node',
    include: ['src/**/*.{test,spec}.{ts,tsx}'],
    // Keep test runs fast and deterministic; no DOM by default. Components that
    // need jsdom can override locally with // @vitest-environment jsdom.
    globals: false,
    coverage: {
      provider: 'v8',
      reporter: ['text', 'html'],
      include: ['src/renderer/**/*.{ts,tsx}', 'src/shared/**/*.ts'],
      exclude: ['src/**/*.test.{ts,tsx}', 'src/**/*.spec.{ts,tsx}'],
    },
  },
})
