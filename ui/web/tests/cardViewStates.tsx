import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { LiveView } from '../src/model/liveViews'
import { CardViewSurface } from '../src/ui/cardViews'

const never = () => new Promise<never>(() => {})
const html = (type: string) => renderToStaticMarkup(
  <CardViewSurface view={new LiveView('session', 'a', 'chart', type, 0, 0, 0)}
    read={never} readBytes={never} send={async () => {}} />,
)
assert.match(html('html'), /<iframe.*title="chart"/)
assert.match(html('unsupported'), /role="alert".*unsupported/)
console.log('Card view states passed')
