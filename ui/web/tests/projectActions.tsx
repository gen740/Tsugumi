// The Canvas's button back to Home: named after the menu's action, and left to
// that action while a run is on (it refuses there, with the reason).
import assert from 'node:assert/strict'
import { create } from 'react-test-renderer'
import { L } from '../src/i18n'
import type { ProjectSession } from '../src/model/session'
import type { Workspace } from '../src/model/workspace'
import { builtinCardItems } from '../src/ui/canvas'
import { ProjectActions, tip } from '../src/ui/canvasWorkspace'

const workspace = { showChat: true, set() {} } as unknown as Workspace
const render = (runActive: boolean, onSwitchProject?: () => void) => create(
  <ProjectActions workspace={workspace} session={{ runActive } as unknown as ProjectSession} onSwitchProject={onSwitchProject} />)
const label = tip(L('Home へ戻る'), 'home')
const home = (view: ReturnType<typeof create>) =>
  view.root.findAllByType('button').find(button => button.props['aria-label'] === label)

for (const runActive of [false, true]) {
  let requested = false
  const view = render(runActive, () => { requested = true })
  const button = home(view)
  assert.ok(button)
  assert.equal(button.props.title, label)
  assert.ok(!button.props.disabled)
  button.props.onClick({ stopPropagation() {} })
  assert.ok(requested, 'the canvas delegates the decision to the Home action')
  view.unmount()
}
const detached = render(false)
assert.equal(home(detached), undefined)
detached.unmount()
console.log('project actions behavior: passed')

// Both language Cards and every plot can be selected from Add Card.
const selected = new Set<string>()
const choices = builtinCardItems(template => selected.add(template))
const select = (items: typeof choices) => {
  for (const item of items) {
    if (!item || typeof item === 'string') continue
    item.action?.()
    if (item.submenu) select(item.submenu)
  }
}
select(choices)
for (const template of ['ruby', 'csharp', 'visualization_matplotlib', 'visualization_plotly', 'visualization_altair']) {
  assert.ok(selected.has(template), `Add Card offers ${template}`)
}
