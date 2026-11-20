import { useEffect, useState } from 'react'
import { L } from '../i18n'
import * as H from '../../shared/gen/tsugumi/host'
import { hostCall } from '../wire/host'
import { s } from '../wire/rpc'
import { errorText, ProjectSession } from '../model/session'
import { useObserve } from '../model/observable'
import { Button, Empty, Form, Row, Section, Spinner, TextField } from './controls'

// Card secrets live in hostd's secret store, beside the API keys, bound to one
// project id. This pane sets, replaces and deletes the open project's values;
// hostd never sends a value back.
const cardSecrets = (type: H.RequestPayload, value: H.RequestT['value']) =>
  hostCall<H.CardSecretListT>(type, value, H.ReplyPayload.CardSecretList)

const NAME = /^[a-z][a-z0-9_]{0,63}$/

export function CardSecretsSettings({ session }: { session: ProjectSession }) {
  useObserve(session)
  const project = session.projectId
  const [names, setNames] = useState<string[]>([])
  const [name, setName] = useState('')
  const [value, setValue] = useState('')
  const [busy, setBusy] = useState(false)
  const [problem, setProblem] = useState<string>()
  const [message, setMessage] = useState<string>()
  const adopt = (list: H.CardSecretListT) => setNames(list.names.map((n) => s(n)))
  useEffect(() => {
    if (!project) return
    void cardSecrets(H.RequestPayload.CardSecretListRequest, new H.CardSecretListRequestT(project)).then(adopt, (e) => setProblem(errorText(e)))
  }, [project])
  async function perform(action: () => Promise<void>) {
    setBusy(true)
    setProblem(undefined)
    setMessage(undefined)
    try { await action() }
    catch (e) { setProblem(errorText(e)) }
    finally { setBusy(false) }
  }
  const valid = NAME.test(name) && value.length > 0
  if (!project) return <Empty small icon="key" title={L('プロジェクトの実行環境に接続すると設定できます')} />

  return <Form>
    <Section title={L('Card シークレット')} info={L('Card の定義（_card.json の "secrets"）が名前を挙げたシークレットを、その Card のプロセスに同じ名前の環境変数として渡します。値はこのホストの hostd がこのプロジェクト用にだけ保存し、表示されることはありません。')}>
      {names.length === 0 && <Empty small icon="key" title={L('設定されたシークレットはありません')} />}
      {names.map((n) => <Row key={n} label={n}>
        <span className="caption secondary">{L('設定済み')}</span>
        <Button small disabled={busy} onClick={() => { setName(n); setValue(''); setMessage(undefined) }}>{L('置き換え')}</Button>
        <Button small kind="destructive" disabled={busy} onClick={() => void perform(async () => {
          adopt(await cardSecrets(H.RequestPayload.CardSecretSetRequest, new H.CardSecretSetRequestT(project!, n, null, true)))
          setMessage(L('{0} を削除しました。', n))
        })}>{L('削除')}</Button>
      </Row>)}
    </Section>
    <Section title={L('シークレットを設定')} info={L('Card がその値をどう扱うか（子プロセスへの継承、ログ、出力）は Card の作者の責任です。')}>
      <Row label={L('名前')} help={L('小文字、数字、_ で 64 文字まで（先頭は小文字）。')}>
        <TextField value={name} mono placeholder="database_password" disabled={busy} onChange={(v) => { setName(v); setMessage(undefined) }} />
      </Row>
      <Row label={L('値')}>
        <input className="field mono" type="password" autoComplete="new-password" spellCheck={false} value={value} disabled={busy}
          aria-label={L('値')} onChange={(e) => { setValue(e.target.value); setMessage(undefined) }} />
      </Row>
      <Row>
        <Button kind="prominent" disabled={!valid || busy} onClick={() => void perform(async () => {
          adopt(await cardSecrets(H.RequestPayload.CardSecretSetRequest, new H.CardSecretSetRequestT(project!, name, value, false)))
          setValue('')
          setMessage(L('{0} を保存しました。', name))
        })}>{names.includes(name) ? L('置き換え') : L('保存')}</Button>
        {busy && <Spinner small />}
      </Row>
    </Section>
    {(problem || message) && <Row><span className="caption selectable" role={problem ? 'alert' : undefined} style={{ color: problem ? 'var(--red)' : undefined }}>{problem ?? message}</span></Row>}
  </Form>
}
