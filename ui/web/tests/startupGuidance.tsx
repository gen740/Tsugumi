import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import * as H from '../shared/gen/tsugumi/host'
import { translate } from '../shared/i18n'
import { ProjectSession } from '../src/model/session'
import { Workspace } from '../src/model/workspace'
import { environmentFailureText, WorkflowTabs } from '../src/ui/workbench'
import { environmentState } from '../src/wire/environment'

const path = 'definitions/record/_card.json'
const message = `${path}: workerCapabilities is not allowed on type code`
const failed = Object.assign(new H.EnvironmentStateT(), {
  phase: H.EnvironmentPhase.Failed, message: 'tsugumid exited.',
  startupError: Object.assign(new H.StartupErrorT(), { kind: 'invalid_document', path, message }),
})
const session = new ProjectSession('/project', 'key', 'Project')
const workspace = new Workspace()
session.environment = environmentState(failed)
const markup = renderToStaticMarkup(<WorkflowTabs session={session} workspace={workspace} />)
assert.ok(markup.includes(`プロジェクト内の「${path}」は、このバージョンの Tsugumi では無効です。`))
assert.ok(markup.includes(message))
assert.ok(markup.includes('エラーメッセージで指摘された箇所を修正または削除してから、再試行してください。'))
assert.ok(!markup.includes('ログを確認してから、再試行してください。'))

const en = (text: string, ...values: unknown[]) => translate('en', text, values)
assert.deepEqual(environmentFailureText(session.environment, en), {
  message: `The project file “${path}” is invalid for this version of Tsugumi.\n${message}`,
  recovery: 'Correct or remove what the error message identifies, then retry.',
})

failed.startupError = null
failed.log = 'time error cannot open the project: permission denied'
session.environment = environmentState(failed)
const generic = renderToStaticMarkup(<WorkflowTabs session={session} workspace={workspace} />)
assert.ok(generic.includes('ログを確認してから、再試行してください。'))
assert.equal(environmentFailureText(session.environment, en).recovery, 'Check the log, then retry.')
session.dispose()
