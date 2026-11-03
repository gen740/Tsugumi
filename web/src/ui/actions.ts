// Run entry points shared by the canvas control, menus and shortcuts.
import { app } from '../model/app'
import type { Stage } from '../model/canvasKit'
import { CardKey, errorText, key, path, ProjectSession } from '../model/session'
import type { Workspace, WorkflowTab } from '../model/workspace'
import { get, str } from '../shared/json'

/** The whole document runs from its root trigger, whichever scope is shown. */
export function runAll(s: ProjectSession) {
  const trigger = s.cards([]).find((c) => {
    const id = str(get(c, 'id'))
    return id !== undefined && s.kind(key([id])) === 'trigger'
  })
  const id = str(get(trigger, 'id'))
  if (!id) {
    s.notice = '全体を実行するには最上位の Workflow に開始 Card（trigger）を置いてください。Card を選んで個別に実行もできます。'
    s.changed()
    return
  }
  void (async () => {
    if (s.computationDirty) await s.save()
    try {
      await s.run([id], true)
    } catch (e) {
      s.notice = errorText(e)
      s.changed()
    }
  })()
}

export function runCard(s: ProjectSession, card: CardKey, propagate: boolean) {
  void (async () => {
    if (s.computationDirty) await s.save()
    try {
      await s.run(path(card), propagate)
    } catch (e) {
      s.notice = errorText(e)
      s.changed()
    }
  })()
}

/** Double click: a Card in Card opens in place, a Draft in its editor, others in settings. */
export function openCard(s: ProjectSession, k: CardKey, tab: WorkflowTab) {
  switch (s.kind(k)) {
    case 'subgraph': tab.enter(path(k)); break
    case 'draft': app.openWindow(s, { kind: 'draft', card: k }); break
    default: app.openWindow(s, { kind: 'card', card: k })
  }
}

/** The log of a Card in the bottom panel, at a stage and invocation when the
 *  workspace takes them (Workspace.showLog(card, stage?, invocation?)). */
export function showLog(workspace: Workspace, card: CardKey | undefined, stage?: Stage, invocation?: bigint) {
  ;(workspace.showLog as (card: CardKey | undefined, stage?: Stage, invocation?: bigint) => void).call(workspace, card, stage, invocation)
}

/** Dragging a definition (or a template) onto the canvas carries this prefix. */
export const DEFINITION_DRAG = 'tsugumi-definition:'
/** A template ("tsugumi-template:python"). */
export const TEMPLATE_DRAG = 'tsugumi-template:'
/** A definition of an external project: "<prefix>\n<id>". */
export const EXTERNAL_DEFINITION_DRAG = 'tsugumi-external-definition:'
/** A Card in Card's external project to copy ("externals/<name>/"). */
export const EXTERNAL_DRAG = 'tsugumi-external:'

/** A child workflow in a window of its own (a browser window on the same core). */
export function openWorkflowWindow(session: ProjectSession, scope: string[]) {
  const u = new URL(location.href)
  u.search = ''
  u.searchParams.set('project', session.dir)
  u.searchParams.set('scope', scope.join('/'))
  window.open(u.toString(), '_blank', 'popup,width=1100,height=720')
}
