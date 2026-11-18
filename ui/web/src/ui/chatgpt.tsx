import { useEffect, useState } from 'react'
import { api } from '../api'
import { L } from '../i18n'
import { ChatGPTSignInModel, ChatGPTState } from '../model/chatgpt'
import { useObserve } from '../model/observable'
import { Button, Row, Section, Select, Sheet, TextField } from './controls'
import { Icon } from './icons'

/** Shared Settings/first-start presentation. Tokens and the welcome record
 *  stay in the host service. */
export function ChatGPTSettings({ model = '', setModel, inUse, onboarding = false, onConnected, onChanged, controller }: {
  model?: string; setModel?: (m: string) => void; inUse?: boolean; onboarding?: boolean
  onConnected: () => void | Promise<void>; onChanged?: () => void; controller?: ChatGPTSignInModel
}) {
  const [connection] = useState(() => controller ?? new ChatGPTSignInModel())
  const [models, setModels] = useState<{ id: string; label: string }[]>([])
  useObserve(connection)
  connection.onConnected = onConnected
  useEffect(() => {
    connection.mount()
    void connection.run(async () => { await connection.reload(); return connection.message })
    const timer = setInterval(() => { if (connection.state?.signingIn) void connection.poll() }, 1000)
    return () => { clearInterval(timer); connection.dispose() }
  }, [connection])
  const st = connection.state
  if (!st) return null
  const active = st.accounts.find((a) => a.active)
  return (
    <>
      <Section title={L('ChatGPT プラン（Sign in with ChatGPT）{0}', inUse ? '（使用中）' : '')}>
        {!onboarding && <Row><span className="callout secondary" style={{ whiteSpace: 'normal' }}>{L('ChatGPT の Plus・Pro プランで Agent を使います。リクエストは OpenAI の Responses API に送られ、プランの利用枠から使われます（API キーの課金とは別で、Tsugumi に料金はかかりません）。Plus の 5 時間ごとの利用上限は、ほかのアプリ（オープンソースのツールを含む）と共有です。Pro にはこの上限はありません。')}</span></Row>}
        {st.connectionMessage && <Row><span className="caption selectable">{st.connectionMessage}</span></Row>}
        {!onboarding && active && <Row label={L('使用中のアカウント')}><span className="selectable">{active.email ?? active.clientId}</span></Row>}
        {active?.signedIn && !active.planEnabled && (
          <Row><span className="caption" style={{ color: 'var(--orange)', whiteSpace: 'normal' }}>{L('このアカウントでは ChatGPT プランの利用が許可されていないため、Agent は使えません。許可するか、API キーの接続を選んでください。')}</span></Row>
        )}
        {st.accounts.filter(a => !onboarding || a.active).map((a) => (
          <Row key={a.clientId} label={<span><Icon name={a.active ? 'checkmark.circle.fill' : 'circle'} /> {a.email ?? a.clientId}</span>}>
            <div className="row gap8 wrap center">
              <span className="caption secondary">{!a.signedIn ? L('ChatGPT にもう一度サインインしてください。') : a.planEnabled ? L('サインイン中') : L('サインイン中（プランの利用は未許可）')}</span>
              {!onboarding && a.signedIn && !a.active && <Button small onClick={() => void connection.run(async () => { connection.setState(await api<ChatGPTState>('/api/chatgpt/select', { clientId: a.clientId })); onChanged?.(); return undefined })}>{L('このアカウントを使う')}</Button>}
              {!a.signedIn && <ChatGPTButton disabled={st.signingIn || !connection.local} onClick={() => connection.signIn(a.clientId)} />}
              {a.signedIn && !a.planEnabled && <Button small disabled={st.signingIn || !connection.local} onClick={() => connection.signIn(a.clientId, true)}>{L('プランの利用を許可')}</Button>}
              {!onboarding && a.signedIn && <Button small kind="destructive" onClick={() => void connection.run(async () => {
                const r = await api<ChatGPTState & { message: string }>('/api/chatgpt/signout', { clientId: a.clientId })
                connection.setState(r)
                onChanged?.()
                return r.message
              })}>{L('サインアウト')}</Button>}
            </div>
          </Row>
        ))}
        <Row>
          <div className="row gap8 wrap center">
            {st.signingIn
              ? <>
                  <span className="caption secondary">{L('ブラウザでサインインを続けてください…')}</span>
                  {connection.url && <a className="caption" href={connection.url} target="_blank" rel="noopener noreferrer">{L('サインインのページを開く')}</a>}
                  <Button small onClick={() => void connection.cancel()}>{L('中止')}</Button>
                </>
              : <ChatGPTButton disabled={!connection.local} onClick={() => connection.signIn()} />}
            <a href={st.usageURL} target="_blank" rel="noopener noreferrer">Manage usage</a>
          </div>
        </Row>
        {!connection.local && <Row><span className="caption secondary" role="status">{L('ChatGPT のサインインは、Web UI のサーバーと同じマシンで localhost または 127.0.0.1 のページを開いて行ってください。ここでは API キーを使えます。')}</span></Row>}
        {!onboarding && <Row><span className="caption secondary" style={{ whiteSpace: 'normal' }}>{L('サインインはこのブラウザで行い、Web UI のサーバーが 127.0.0.1 で結果を受け取ります（サーバーと同じマシンのブラウザで開いてください）。トークンはこのサーバーのデータフォルダのファイル（0600）に置かれ、ページ・core・Card には渡されず、プロジェクト・設定・ログにも残りません。macOS アプリのサインインとは別です。')}</span></Row>}
      </Section>
      {!onboarding && <Section title={L('モデル')}>
        <Row label={L('モデル')}><TextField value={model} onChange={setModel ?? (() => {})} /></Row>
        <Row>
          <Button disabled={!active?.signedIn} onClick={() => void connection.run(async () => {
            const r = await api<{ models: string[]; modelNames: Record<string, string> }>('/api/settings/models', { provider: 'chatgpt' })
            setModels(r.models.map((id) => ({ id, label: r.modelNames[id] ?? id })))
            return L('{0} 件のモデルを取得しました。', r.models.length)
          })}>{L('モデル一覧を取得')}</Button>
        </Row>
        {models.length > 0 && <Row label={L('一覧から選択')}><Select value={model} onChange={setModel ?? (() => {})} options={models.map((m) => ({ value: m.id, label: m.label }))} /></Row>}
      </Section>}
      {connection.message && <div className="caption selectable">{connection.message}</div>}
      {st.outcome?.welcome && (
        <Sheet width={420} onClose={() => connection.welcomed(false)}>
          <div className="sheet-body">
            <b>{L('ChatGPT プランを使っています')}</b>
            <span className="callout secondary" style={{ whiteSpace: 'normal' }}>{L('Agent のリクエストは ChatGPT プランの利用枠から使われます。利用状況は ChatGPT の設定（Usage）で確認・管理できます。')}</span>
            <div className="row gap8" style={{ justifyContent: 'flex-end' }}>
              <Button onClick={() => connection.welcomed(true)}>Manage usage</Button>
              <Button kind="prominent" onClick={() => connection.welcomed(false)}>Got it</Button>
            </div>
          </div>
        </Sheet>
      )}
    </>
  )
}


export function ChatGPTButton({ disabled, onClick }: { disabled?: boolean; onClick: () => void }) {
  return <button type="button" className="btn" data-oai-siwc-button="" disabled={disabled}
    style={{ background: '#000', color: '#fff', border: '1px solid #000', borderRadius: 6, padding: '10px 16px' }}
    onClick={onClick}>Continue with ChatGPT</button>
}
