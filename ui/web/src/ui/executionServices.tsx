import { useEffect, useState } from 'react'
import { L } from '../i18n'
import * as H from '../../shared/gen/tsugumi/host'
import { hostCall } from '../wire/host'
import { s } from '../wire/rpc'
import { errorText } from '../model/session'
import { Button, Form, Row, Section, Spinner, TextField, Value } from './controls'

const executionServices = (type: H.RequestPayload, value: H.RequestT['value']) =>
  hostCall<H.ExecutionServicesT>(type, value, H.ReplyPayload.ExecutionServices)

interface Registration { id: string; name: string; endpoint: string; workerPublicKey: string; grade: string }

const registration = (entry: H.ExecutionServiceT): Registration => ({
  id: s(entry.id), name: s(entry.name), endpoint: s(entry.endpoint),
  workerPublicKey: s(entry.workerPublicKey), grade: String(entry.grade),
})

export function ExecutionServicesSettings({ changed }: { changed: () => void }) {
  const [hostPublicKey, setHostPublicKey] = useState('')
  function adopt(state: H.ExecutionServicesT) { setEntries(state.services); setHostPublicKey(s(state.hostPublicKey)) }
  const [entries, setEntries] = useState<H.ExecutionServiceT[]>([])
  const [edit, setEdit] = useState<Registration>()
  const [original, setOriginal] = useState<string>()
  const [busy, setBusy] = useState(false)
  const [problem, setProblem] = useState<string>()
  const [message, setMessage] = useState<string>()
  useEffect(() => { void executionServices(H.RequestPayload.ExecutionServicesRequest, new H.ExecutionServicesRequestT()).then(adopt, (e) => setProblem(errorText(e))) }, [])

  function choose(entry: Registration, id?: string) {
    const { name, endpoint, workerPublicKey, grade, id: serviceId } = entry
    setEdit({ id: serviceId, name, endpoint, workerPublicKey, grade })
    setOriginal(id)
    setProblem(undefined)
    setMessage(undefined)
  }
  function update(field: keyof Registration, value: string | number) {
    setEdit((previous) => previous && {
      ...previous,
      [field]: value,
    })
    setMessage(undefined)
  }
  async function perform(action: () => Promise<void>) {
    setBusy(true)
    setProblem(undefined)
    setMessage(undefined)
    try { await action() }
    catch (e) { setProblem(errorText(e)) }
    finally { setBusy(false) }
  }
  const saved = entries.find((entry) => s(entry.id) === original)
  const sameEndpoint = !!saved && s(saved.endpoint) === edit?.endpoint.trim()
  const caps = sameEndpoint && saved.capabilities?.serverId ? saved.capabilities : undefined
  const valid = !!edit && [edit.id, edit.name, ...(edit.id === 'local' ? [] : [edit.endpoint, edit.workerPublicKey])].every((value) => value.trim())
    && /^(0x[0-9a-f]+|[0-9]+)$/i.test(edit.grade) && BigInt(edit.grade) <= 0xffffffffffffffffn

  return <Form>
    <Section title={L('このホストの公開鍵')} info={L('ホストの公開鍵を実行デーモンの authorized_keys に追加してから、接続を確認してください。')}>
      <Row><code className="selectable">{hostPublicKey}</code>
        <Button disabled={!hostPublicKey} onClick={() => void perform(async () => { await navigator.clipboard.writeText(hostPublicKey) })}>{L('公開鍵をコピー')}</Button></Row>
    </Section>
    <Section title={L('実行サービス')}>
      {entries.map((entry) => <Row key={s(entry.id)}>
        <Button kind="borderless" disabled={busy} onClick={() => choose(registration(entry), s(entry.id))}>{s(entry.name)} ({s(entry.id)})</Button>
        <span className="secondary">{s(entry.capabilities!.nativeSystem)}</span>
      </Row>)}
      <Row><Button icon="plus" disabled={busy} onClick={() => choose({ id: '', name: '', endpoint: '', workerPublicKey: '', grade: '1' })}>{L('実行サービスを追加')}</Button></Row>
    </Section>
    {edit && <Section>
      <Row label={L('名前')}><TextField value={edit.name} disabled={busy} onChange={(value) => update('name', value)} /></Row>
      <Row label={L('サービス ID')}><TextField value={edit.id} mono disabled={busy || !!original} onChange={(value) => update('id', value)} /></Row>
      <Row label={L('ワーカーの grade')} help={L('64 bit の符号なし整数（10 進数または 0x から始まる 16 進数）です。条件を満たす空きワーカーのうち、最小の値を選びます。')}><TextField value={edit.grade} mono disabled={busy} onChange={(value) => update('grade', value)} /></Row>
      <Row label={L('実行デーモンの接続先')} help={L('TLS 接続先を入力してください（例: tls://worker.example:8765）。')}>
        <TextField value={edit.endpoint} placeholder="tls://worker.example:8765" mono disabled={busy || edit.id === 'local'} onChange={(value) => update('endpoint', value)} />
      </Row>
      <Row label={L('実行デーモンの公開鍵')} help={L('公開鍵は ed25519: で始まる SPKI 形式です。')}>
        <TextField value={edit.workerPublicKey} mono disabled={busy || edit.id === 'local'} onChange={(value) => update('workerPublicKey', value)} />
      </Row>
      {caps && <>
        <Value label={L('CPU アーキテクチャ')} value={caps.systems.join(', ')} />
        <Value label={L('隔離方式')} value={L('PID 名前空間')} />
        <Value label={L('CPU とメモリの上限')} value={caps.resourceLimits ? L('対応') : L('非対応')} />
        <Value label={L('最大同時ジョブ数')} value={String(caps.maxConcurrentJobs)} />
      </>}
      <Row>
        <Button kind="prominent" disabled={!valid || busy} onClick={() => void perform(async () => {
          const next = await executionServices(H.RequestPayload.ExecutionServicePutRequest,
            new H.ExecutionServicePutRequestT(edit.id, edit.name, edit.endpoint, edit.workerPublicKey, !!original, BigInt(edit.grade)))
          const registered = next.services.find((entry) => s(entry.id) === edit.id)
          if (!registered) throw Error(L('実行サービスを登録できませんでした。'))
          adopt(next)
          choose(registration(registered), s(registered.id))
          setMessage(original ? L('接続を確認しました。') : L('公開鍵を登録しました。authorized_keys にホストの公開鍵を追加してから接続を確認してください。'))
          changed()
        })}>{original ? L('接続を確認して更新') : L('登録')}</Button>
        {original && original !== 'local' && <Button kind="destructive" disabled={busy} onClick={() => void perform(async () => {
          adopt(await executionServices(H.RequestPayload.ExecutionServiceRemoveRequest, new H.ExecutionServiceRemoveRequestT(original)))
          setEdit(undefined)
          setOriginal(undefined)
          changed()
        })}>{L('削除')}</Button>}
        {busy && <Spinner small />}
      </Row>
    </Section>}
    {(problem || message) && <Row><span className="caption selectable" style={{ color: problem ? 'var(--red)' : undefined }}>{problem ?? message}</span></Row>}
  </Form>
}
