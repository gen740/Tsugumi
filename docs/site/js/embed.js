// A live Canvas in the text of a document (<div data-player="card-guide"></div>).
const still = matchMedia('(prefers-reduced-motion: reduce)').matches
const hosts = [...document.querySelectorAll('[data-player]')]

if (hosts.length) {
  try {
    const { mountPlayer } = await import('./player.js')
    for (const el of hosts) {
      el.classList.add('card-embed')
      const id = el.dataset.player
      mountPlayer(el, { demo: id, data: new URL(`../player-data/${id}.json`, import.meta.url).href, autoplay: !still })
    }
  } catch {
    for (const el of hosts) el.hidden = true  // the steps in the text stand on their own
  }
}
