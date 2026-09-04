import { existsSync } from 'node:fs'
import path from 'node:path'

// e2e used to skip every test when the build was missing, reporting green
// with zero tests executed. Fail loudly instead.
export default function globalSetup(): void {
  const mainJs = path.join(__dirname, '..', 'out', 'main', 'main.js')
  if (!existsSync(mainJs)) {
    throw new Error('out/main/main.js missing — run `npm run build` before `npm run test:e2e`')
  }
}
