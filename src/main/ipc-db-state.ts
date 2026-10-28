/** Shared scan DB init state for IPC handlers split across modules. */

let dbReady = false
let dbInitError: string | null = null

export function setDbInitResult(ready: boolean, error: string | null): void {
  dbReady = ready
  dbInitError = error
}

export function getDbStatusPayload(): { ready: boolean; error?: string } {
  return { ready: dbReady, error: dbInitError ?? undefined }
}

export function isDbReady(): boolean {
  return dbReady
}

export function dbInitErrorMessage(): string | null {
  return dbInitError
}

export function assertDbReady(): void {
  if (!dbReady) {
    throw new Error(dbInitError ?? 'Veritabanı kullanılamıyor')
  }
}
