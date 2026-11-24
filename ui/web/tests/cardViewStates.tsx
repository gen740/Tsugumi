// A view window waits for the declarations before it says there are none.
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { CardLiveViews } from '../src/model/liveViews'
import { CardViewsPanel } from '../src/ui/cardViews'

const never = () => new Promise<never>(() => {})
const html = (card?: CardLiveViews) =>
  renderToStaticMarkup(<CardViewsPanel card={card} read={never} readBytes={never} send={async () => {}} />)
const loading = (markup: string) => markup.includes('role="progressbar"')
const none = (markup: string) => markup.includes('表示するビューがありません')

assert.ok(loading(html()) && !none(html()), 'a Card without live state is loading')
const card = new CardLiveViews()
card.data('early', Uint8Array.of(1))
assert.ok(loading(html(card)) && !none(html(card)), 'data before the declarations does not end the loading')
card.declare([])
assert.ok(!loading(html(card)) && html(card).includes('early'), 'after the declarations, undeclared data is reported')
const empty = new CardLiveViews()
empty.declare([])
assert.ok(none(html(empty)) && !loading(html(empty)), 'evaluated to an empty list: no views')
card.declare([{ name: 'game', type: 'dawn', blob: '/nix/store/game' }])
assert.ok(!none(html(card)) && !loading(html(card)) && html(card).includes('game'), 'a declared view is shown')
card.clear()
assert.ok(loading(html(card)), 'cleared declarations are being evaluated again')
card.declare([], 'broken')
assert.ok(html(card).includes('broken') && !loading(html(card)), 'a failed evaluation is shown, not loading')
console.log('Card view states passed')
