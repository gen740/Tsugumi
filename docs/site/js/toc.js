// A long page's list of headings: the heading being read is marked, and kept in view.
const links = [...document.querySelectorAll('.toc a')]
const targets = links.map((a) => document.getElementById(decodeURIComponent(a.hash.slice(1))))
const list = document.querySelector('.toc')
let current = null

function mark() {
  const line = window.innerHeight * 0.25
  let i = targets.findIndex((h) => h && h.getBoundingClientRect().top > line) - 1
  if (i === -2) i = targets.length - 1
  const on = links[Math.max(0, i)]
  if (on === current) return
  current?.classList.remove('on')
  on.classList.add('on')
  current = on
  if (list.scrollHeight > list.clientHeight) list.scrollTop = on.offsetTop - list.clientHeight / 2
}

if (links.length) {
  let queued = false
  addEventListener('scroll', () => {
    if (queued) return
    queued = true
    requestAnimationFrame(() => { queued = false; mark() })
  }, { passive: true })
  addEventListener('resize', mark)
  mark()
}
