/** Clamp the page when a refreshed result shrinks. */
export function tablePage(rows: number, requested: number) {
  const size = 200
  const count = Math.max(1, Math.ceil(rows / size))
  const index = Math.min(Math.max(0, requested), count - 1)
  const start = index * size
  return { index, count, start, end: Math.min(rows, start + size) }
}
