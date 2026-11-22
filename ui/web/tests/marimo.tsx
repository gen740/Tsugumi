import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { ProjectSession } from '../src/model/session'
import { parse } from '../shared/json'
import { D } from '../src/wire/rpc'
import { CardEditorModel } from '../src/ui/cardSettings'
import { FailureSourceLink } from '../src/ui/history'

const session = new ProjectSession('/p', 'marimo', 'marimo')
session.project = parse('{"canvas":{"cards":[{"id":"notebook"}],"connections":[]}}')
session.confirmedDefinitions.set('notebook', parse('{"type":"code"}'))
const model = new CardEditorModel(session, 'notebook')
const source = 'definitions/notebook/src/notebook.py'
model.texts.set(source, 'unsaved notebook changes')
const failure = new D.FailureT()
failure.args = [['source', 'src/notebook.py'], ['line', '8'], ['cell', 'failing_cell']].map(([key, value]) => Object.assign(new D.KeyValueT(), {key, value}))
const link = FailureSourceLink({ failure, onOpenSource: (source, line) => model.openSource(source, line) })!
assert.match(renderToStaticMarkup(link), /failing_cell.*src\/notebook.py:8/)
link.props.children.props.onClick()
assert.equal(model.category, 'source')
assert.equal(model.shownFile, source)
assert.equal(model.sourceLine, 8)
assert.equal(model.texts.get(source), 'unsaved notebook changes', 'Navigation never rewrites an editor draft')
for (const bad of ['../notebook.py', '/tmp/notebook.py']) model.openSource(bad, 10)
assert.equal(model.shownFile, source)
assert.equal(model.sourceLine, 8)
model.openFile('definitions/notebook/src/main.py')
assert.equal(model.sourceLine, undefined, 'Ordinary file switching clears the failure position')
model.openSource('src/notebook.py', 8)
model.closeFile(source)
assert.equal(model.sourceLine, undefined, 'Closing the failed source does not jump within another file')
failure.args = []
assert.equal(FailureSourceLink({failure, onOpenSource: () => assert.fail('no source position')}), null)
console.log('marimo: failure cell link opens the source line and keeps unsaved edits')
