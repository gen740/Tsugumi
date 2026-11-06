// The Canvas's button back to Home: named after the menu's action, and left to
// that action while a run is on (it refuses there, with the reason).
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import type { ProjectSession } from '../src/model/session'
import type { Workspace } from '../src/model/workspace'
import { ProjectActions, tip } from '../src/ui/canvasWorkspace'

const workspace = { showChat: true, set() {} } as unknown as Workspace
const render = (runActive: boolean, onSwitchProject?: () => void) => renderToStaticMarkup(
  <ProjectActions workspace={workspace} session={{ runActive } as unknown as ProjectSession} onSwitchProject={onSwitchProject} />)
const label = tip('Home へ戻る', 'home')

for (const runActive of [false, true]) {
  const html = render(runActive, () => {})
  const button = html.split('<button').find((b) => b.includes(`aria-label="${label}"`))?.split('>')[0]
  assert.ok(button, 'an icon button with the label of the action')
  assert.ok(button.includes(`title="${label}"`), 'a tooltip with its shortcut')
  assert.ok(!button.includes('disabled'), 'the action itself refuses during a run, as from the menu')
}
// A child Workflow in a window of its own has no Home to go back to.
assert.ok(!render(false).includes(label))
console.log('project actions behavior: passed')
