export function htmlEscape(value: string): string {
  return value
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
}

export function csvCell(value: unknown): string {
  let s = String(value ?? '')
  if (/^[=+\-@]/.test(s)) s = `'${s}`
  if (/[",\n;]/.test(s)) return `"${s.replace(/"/g, '""')}"`
  return s
}
