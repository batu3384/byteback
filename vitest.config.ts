import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'

// Vitest configuration for the renderer/TypeScript layer.
// Main-process code (src/main/**) talks to the native addon and Electron APIs
// and is not unit-tested here; it is covered by integration checks instead.
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
