/**
 * Safe date parsing and formatting helpers for PostgreSQL / Drogon timestamps.
 *
 * PostgreSQL returns two shapes across endpoints:
 * - `TIMESTAMP` (without time zone, stored in UTC): `"2026-04-12 09:00:00"`
 * - `TIMESTAMPTZ` (with time zone, e.g. `scheduled_at`): `"2026-04-12 09:00:00+00"`
 *   or ISO 8601 `"2026-04-12T09:00:00.000Z"`
 *
 * Blindly doing `s.replace(' ', 'T') + 'Z'` turns `"2026-04-12 09:00:00+00"`
 * into `"2026-04-12T09:00:00+00Z"`, which is `Invalid Date` in all engines.
 */
export function parseApiDate(raw?: string | null): Date | null {
  if (!raw) return null
  let s = raw.trim()
  if (!s) return null

  // Replace space separator between YYYY-MM-DD and HH:MM:SS with 'T'.
  s = s.replace(/^(\d{4}-\d{2}-\d{2})\s+/, '$1T')

  // Normalize short PostgreSQL hour-only offset (`+00` -> `+00:00`) for WebKit.
  if (/[+-]\d{2}$/.test(s)) {
    s = `${s}:00`
  } else if (!/(?:Z|[+-]\d{2}:\d{2}|[+-]\d{4})$/i.test(s)) {
    // Naive timestamp from PostgreSQL (stored as UTC).
    s = `${s}Z`
  }

  const d = new Date(s)
  return Number.isNaN(d.getTime()) ? null : d
}

export function formatDateTime(raw?: string | null): string {
  if (!raw) return ''
  const d = parseApiDate(raw)
  return d ? d.toLocaleString() : raw
}

export function formatDateShort(
  raw?: string | null,
  options: Intl.DateTimeFormatOptions = { month: 'short', day: 'numeric', year: 'numeric' },
): string {
  if (!raw) return ''
  const d = parseApiDate(raw)
  return d ? d.toLocaleDateString(undefined, options) : raw
}

export function isoDate(raw?: string | null): string {
  const d = parseApiDate(raw)
  return d ? d.toISOString() : ''
}

export function toDatetimeLocalValue(raw?: string | null): string {
  const d = parseApiDate(raw)
  if (!d) return ''
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`
}
