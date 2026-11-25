import { useEffect, useState } from 'react'
import * as H from '../../shared/gen/tsugumi/host'
import { arr, get, obj, str } from '../../shared/json'
import { L } from '../i18n'
import { validName } from '../model/names'
import { errorText, ProjectSession } from '../model/session'
import { hostCall } from '../wire/host'
import { s } from '../wire/rpc'
import { Button, Form, Row, Section, Select, TextField } from './controls'
import { FolderPicker } from './sidebar'

async function setBinding(session: ProjectSession, scope: string, name: string, folder?: string) {
  const project = session.projectId
  if (!project) throw new Error(L('接続していません'))
  return session.withEnvironmentChange(() => hostCall<H.HostPathBindingsT>(
    H.RequestPayload.HostPathSetRequest,
    new H.HostPathSetRequestT(project, scope || null, name, folder ?? null, folder === undefined),
    H.ReplyPayload.HostPathBindings))
}

function declarations(session: ProjectSession, scope: string) {
  const project = scope ? session.external(scope)?.project : session.project
  return arr(get(project, 'hostPaths')) ?? []
}

function BindPicker({ session, scope, name, onClose, onResult, initial }: {
  session: ProjectSession; scope: string; name: string; initial?: string
  onClose: () => void; onResult: (promise: Promise<H.HostPathBindingsT>) => void
}) {
  const declaration = declarations(session, scope).find(entry => str(get(entry, 'name')) === name)
  const description = str(get(declaration, 'description'))
  const message = L('ホストパス「{0}」のフォルダを選んでください。', name)
  return <FolderPicker initial={initial} message={description ? message + ' ' + description : message}
    onClose={onClose} onPick={folder => { onClose(); onResult(setBinding(session, scope, name, folder)) }} />
}

export function HostPathPrompt({ session }: { session: ProjectSession }) {
  const prompt = session.hostPathPrompt
  if (!prompt) return null
  return <BindPicker {...prompt} session={session} onClose={() => {
    session.hostPathPrompt = undefined
    session.changed()
  }} onResult={promise => {
    void promise.then(() => { session.notice = L('フォルダを割り当てました。もう一度実行してください。') },
      error => { session.notice = errorText(error) }).finally(() => session.changed())
  }} />
}

export function HostPathsSettings({ session }: { session: ProjectSession }) {
  const [scope, setScope] = useState('')
  const [name, setName] = useState('')
  const [description, setDescription] = useState('')
  const [bindings, setBindings] = useState<H.HostPathBindingsT>()
  const [picking, setPicking] = useState<string>()
  const [problem, setProblem] = useState<string>()
  const [busy, setBusy] = useState(false)
  const project = session.projectId
  useEffect(() => {
    if (!project || !session.connected) return
    let current = true
    void hostCall<H.HostPathBindingsT>(H.RequestPayload.HostPathListRequest,
      new H.HostPathListRequestT(project), H.ReplyPayload.HostPathBindings).then(
      result => { if (current) setBindings(result) },
      error => { if (current) setProblem(errorText(error)) })
    return () => { current = false }
  }, [project, session.connected])

  const rows = declarations(session, scope)
  const binding = (n: string) => bindings?.bindings.find(b => s(b.scope) === scope && s(b.name) === n)
  const update = async (remove?: string) => {
    setBusy(true)
    setProblem(undefined)
    try {
      await session.applyMutation(model => {
        const current = declarations(model, scope)
        const target = remove ?? name
        const existing = current.some(row => str(get(row, 'name')) === target)
        const next = remove ? current.filter(row => str(get(row, 'name')) !== target) :
          existing ? current.map(row => str(get(row, 'name')) === target ? obj([['name', target], ['description', description]]) : row) :
            [...current, obj([['name', target], ['description', description]])]
        model.setProjectField('hostPaths', next.length ? next : undefined, scope)
      })
      setName('')
      setDescription('')
    } catch (error) { setProblem(errorText(error)) }
    finally { setBusy(false) }
  }
  const adopt = (promise: Promise<H.HostPathBindingsT>) => {
    setBusy(true)
    setProblem(undefined)
    void promise.then(setBindings, error => setProblem(errorText(error))).finally(() => setBusy(false))
  }

  return <Form>
    <Section title={L('ホストパス')} info={L('名前と用途をプロジェクトに保存します。フォルダの割り当てはこのホストに保存します。')}>
      {session.externals.size > 0 && <Row label={L('プロジェクト')}>
        <Select value={scope} options={[{ value: '', label: L('最上位のプロジェクト') },
          ...[...session.externals.keys()].sort().map(value => ({ value, label: value }))]}
          onChange={value => { setScope(value); setName(''); setDescription('') }} />
      </Row>}
      {rows.map(row => {
        const n = str(get(row, 'name')) ?? ''
        const text = str(get(row, 'description')) ?? ''
        const folder = binding(n)
        return <Row key={n} label={n} help={text}>
          <span className="caption secondary selectable">{folder ? s(folder.path) : L('未割り当て')}</span>
          <Button small disabled={!session.isEditable || busy} onClick={() => { setName(n); setDescription(text) }}>{L('編集')}</Button>
          <Button small disabled={!project || !session.isEditable || busy} onClick={() => setPicking(n)}>{L('フォルダを選ぶ…')}</Button>
          {folder && <Button small disabled={!session.isEditable || busy} onClick={() => adopt(setBinding(session, scope, n))}>{L('割り当てを解除')}</Button>}
          <Button small kind="destructive" disabled={!session.isEditable || busy} onClick={() => void update(n)}>{L('削除')}</Button>
        </Row>
      })}
    </Section>
    <Section>
      <Row label={L('名前')}><TextField value={name} onChange={setName} mono placeholder="saves" /></Row>
      <Row label={L('用途')}><TextField value={description} onChange={setDescription} /></Row>
      <Row><Button kind="prominent" disabled={!session.isEditable || !validName(name) || !description.trim() || busy} onClick={() => void update()}>{L('保存')}</Button></Row>
    </Section>
    {problem && <Row><span role="alert">{problem}</span></Row>}
    {picking && <BindPicker session={session} scope={scope} name={picking} initial={s(binding(picking)?.path) || undefined}
      onClose={() => setPicking(undefined)} onResult={adopt} />}
  </Form>
}
