// Chat: session list or the conversation (answers, progress, input, model
// and send/stop). Usable docked beside the canvas or in its own window.
import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { api } from '../api'
import { app } from '../model/app'
import { canvasFor } from '../model/canvas'
import { ChatController, ChatItem, refLabel, SearchHit, Skill, USAGE_URL } from '../model/chat'
import { useObserve } from '../model/observable'
import { PERMISSIONS, parsePermission, permissionInfo } from '../model/permissions'
import { isMac, prefs } from '../model/prefs'
import { CardKey, errorText, key, path, ProjectSession } from '../model/session'
import { WorkflowTab } from '../model/workspace'
import { Button, copyText, Empty, MenuButton, Sheet, showMenu, Spinner, TextField, Toggle } from './controls'
import { Icon } from './icons'
import { Markdown } from './markdown'
import { ChatChoices, GuideNest } from './guide'
import { guide } from '../model/guide'
import { FolderPicker } from './sidebar'

export function ChatPanel({ session, chat, detached, tab }: { session: ProjectSession; chat: ChatController; detached: boolean; tab?: WorkflowTab }) {
  useObserve(chat, session, tab, prefs)
  const [showList, setShowList] = useState(false)

  return (
    <div className="chat-panel">
      <div className="chat-header">
        {!detached && <GuideNest busy={chat.busy} />}
        <Button kind="borderless" iconOnly icon={showList ? 'bubble.left.and.bubble.right.fill' : 'list.bullet'} help={showList ? '会話に戻る' : '会話の一覧と検索'} onClick={() => setShowList(!showList)} />
        <b className="ellipsis flex1">{showList ? '会話' : chat.sessions.find((x) => x.id === chat.currentId)?.title ?? '新しい会話'}</b>
        <Button kind="borderless" iconOnly icon="square.and.pencil" help="新しい会話" onClick={() => { chat.newSession(); setShowList(false) }} />
        <MenuButton icon="ellipsis.circle" help="その他" items={() => [
          // Only for the conversation that is open, never from the list.
          !showList && { label: '会話を圧縮', disabled: chat.busy || chat.items.length < 6, action: () => chat.compact() },
          !showList && 'divider',
          { label: 'Skill を再読込', action: () => void chat.reloadSkills() },
          !detached && { label: '別ウインドウで開く', action: () => app.openWindow(session, { kind: 'chat' }) },
          'divider',
          { label: '接続設定…', action: () => app.openWindow(undefined, { kind: 'appSettings' }) },
        ]} />
      </div>
      {showList ? <SessionList chat={chat} onPick={() => setShowList(false)} /> : <Conversation session={session} chat={chat} tab={tab} />}
      {chat.permission && <PermissionPrompt key={chat.permission.id} asked={chat.permission.asked} from={chat.permission.from} onAnswer={(allow, always) => chat.answerPermission(allow, always)} />}
    </div>
  )
}

/** Asks whether the Agent may give Cards what its save needs; each kind can
 *  be allowed without asking from now on (as in the settings). */
function PermissionPrompt({ asked, from, onAnswer }: { asked: string[]; from?: string; onAnswer: (allow: boolean, always: string[]) => void }) {
  const who = from ?? 'Agent'
  const [always, setAlways] = useState<string[]>([])
  const requests = asked.map(parsePermission).filter((r) => r !== undefined)
  const kinds = PERMISSIONS.filter((p) => requests.some((r) => r.kind === p.kind))
  return (
    <Sheet width={460} onClose={() => onAnswer(false, [])}>
      <div className="sheet-body permission-prompt">
        <div className="title2"><Icon name="hand.raised" /> {who} が Card の権限を広げようとしています</div>
        <p className="secondary">許可すると、この保存が行われます。許可しなければ何も保存されず、{who} には理由を説明するよう伝えます。</p>
        <ul className="permission-list">
          {requests.map((r) => (
            <li key={r.raw}>
              <Icon name={permissionInfo(r.kind).icon} />
              <div>
                <div>{r.card}: {permissionInfo(r.kind).label}</div>
                {r.folder && <code className="secondary">{r.folder}</code>}
              </div>
            </li>
          ))}
        </ul>
        <div className="secondary small">今後は確認しない（設定の「API」でいつでも変えられます）</div>
        {kinds.map((k) => (
          <Toggle key={k.kind} kind="checkbox" label={k.label} checked={always.includes(k.kind)}
            onChange={(on) => setAlways(on ? [...always, k.kind] : always.filter((x) => x !== k.kind))} />
        ))}
        <div className="row end gap8">
          <Button onClick={() => onAnswer(false, [])}>許可しない</Button>
          <Button kind="prominent" onClick={() => onAnswer(true, always)}>許可する</Button>
        </div>
      </div>
    </Sheet>
  )
}

// ---- Session list ------------------------------------------------------------------

function SessionList({ chat, onPick }: { chat: ChatController; onPick: () => void }) {
  const [query, setQuery] = useState('')
  const [hits, setHits] = useState<SearchHit[]>([])
  useEffect(() => {
    if (!query) return setHits([])
    const t = setTimeout(() => void chat.search(query).then(setHits).catch(() => setHits([])), 200)
    return () => clearTimeout(t)
  }, [query, chat])
  const open = (id: string) => {
    chat.select(id)
    onPick()
  }
  return (
    <div className="chat-list">
      <div className="sidebar-pad"><TextField value={query} onChange={setQuery} placeholder="履歴を検索" /></div>
      <div className="list flex1">
        {!query && chat.sessions.map((x) => (
          <div key={x.id} className="list-row clickable col-start" onClick={() => open(x.id)}
            onContextMenu={(e) => showMenu(e, [{ label: '削除', destructive: true, action: () => void chat.delete(x.id) }])}>
            <div className="ellipsis" style={{ fontWeight: x.id === chat.currentId ? 600 : 400 }}>{x.title}</div>
            <div className="caption secondary">{new Date(x.updatedAt).toLocaleString('ja-JP', { dateStyle: 'medium', timeStyle: 'short' })} · {x.messages} 件</div>
          </div>
        ))}
        {!query && !chat.sessions.length && <Empty title="会話はまだありません" icon="bubble.left" />}
        {query && hits.map((h, i) => (
          <div key={i} className="list-row clickable col-start" onClick={() => open(h.session)}>
            <div className="caption" style={{ fontWeight: 600 }}>{h.title}</div>
            <div className="caption secondary clamp3">{h.snippet}</div>
          </div>
        ))}
        {query && !hits.length && <Empty title={`“${query}” の結果はありません`} icon="magnifyingglass" description="スペルを確認するか、新しく検索してください。" />}
      </div>
    </div>
  )
}

// ---- Conversation ------------------------------------------------------------------

function Conversation({ session, chat, tab }: { session: ProjectSession; chat: ChatController; tab?: WorkflowTab }) {
  useObserve(guide)
  const scroll = useRef<HTMLDivElement>(null)
  const last = chat.items[chat.items.length - 1]
  // Tsugumi's bubble sent the user here: its answer is scrolled to and marked for a moment.
  const answer = [...chat.items].reverse().find((x) => x.role === 'assistant')
  const [flash, setFlash] = useState<string>()
  useEffect(() => {
    if (!guide.chatFlash || !answer) return
    setFlash(answer.id)
    document.getElementById(`chat-item-${answer.id}`)?.scrollIntoView({ block: 'start', behavior: 'smooth' })
    const t = setTimeout(() => setFlash(undefined), 2200)
    return () => clearTimeout(t)
  }, [guide.chatFlash])
  // Follows the end while the conversation grows.
  useLayoutEffect(() => {
    const el = scroll.current
    if (el) el.scrollTop = el.scrollHeight
  }, [chat.items.length, last?.text, chat.busy])

  return (
    <>
      <div className="chat-scroll" ref={scroll}>
        <div className="chat-column">
          {chat.items.length === 0 && (
            <div className="chat-intro">
              <div className="callout">{guide.available ? 'ツグミです。Workflow の相談・生成・編集も、使い方の質問もどうぞ。' : 'Workflow の相談・生成・編集を依頼できます。'}</div>
              <div className="caption secondary">Card を右クリックして参照に追加、$ で Skill を選べます。</div>
            </div>
          )}
          {chat.items.map((it) => (
            <div key={it.id} id={`chat-item-${it.id}`} className={flash === it.id ? 'guide-flash' : undefined}>
              <Row item={it} streaming={chat.busy && it === last} session={session} tab={tab} />
            </div>
          ))}
          {chat.busy && (
            <div className="chat-activity">
              <Spinner small />
              <span className="caption secondary ellipsis">{currentStep(chat.items)}</span>
            </div>
          )}
        </div>
      </div>
      {chat.status && (
        <div className="chat-status">
          <Icon name={chat.failed ? 'exclamationmark.triangle.fill' : 'info.circle'} style={{ color: chat.failed ? 'var(--red)' : 'var(--secondary)' }} />
          <span className="caption clamp3 selectable flex1" style={{ color: chat.failed ? 'var(--red)' : 'var(--secondary)' }}>{chat.status}</span>
          {chat.usageLimited
            ? <Button small kind="prominent" onClick={() => window.open(USAGE_URL, '_blank', 'noopener,noreferrer')}>Manage usage</Button>
            : chat.failed && <Button small onClick={() => chat.retry()}>再試行</Button>}
          {chat.stoppedAtLimit && !chat.busy && <Button small onClick={() => chat.continueWork()}>続ける</Button>}
        </div>
      )}
      <ChatChoices chat={chat} session={session} tab={tab} />
      <Composer session={session} chat={chat} tab={tab} />
    </>
  )
}

/** What the agent is doing right now (no internal reasoning, only the step). */
function currentStep(items: ChatItem[]): string {
  const t = [...items].reverse().find((x) => x.role === 'tool' && x.tool?.ok === undefined)
  if (t?.tool) return toolStep(t.tool.name, t.text).running
  if (items[items.length - 1]?.role === 'assistant') return '回答を書いています…'
  return '考えています…'
}

function Row({ item, streaming, session, tab }: { item: ChatItem; streaming: boolean; session: ProjectSession; tab?: WorkflowTab }) {
  const op = item.archived ? 0.6 : 1
  switch (item.role) {
    case 'user':
      return <div className="chat-user-row" style={{ opacity: op }}><div className="chat-user selectable">{item.text}</div></div>
    case 'tool':
      return <div style={{ opacity: op }}><ToolRow item={item} session={session} tab={tab} /></div>
    case 'helper':
      return <HelperRow item={item} />
    case 'note':
      return <div className="chat-note caption secondary"><Icon name="rectangle.compress.vertical" size={11} />{item.text}</div>
    default:
      return streaming ? <StreamingText text={item.text} /> : <div style={{ opacity: op }}><ChatMarkdown text={item.text} /></div>
  }
}

// ---- Composer ----------------------------------------------------------------------

function Composer({ session, chat, tab }: { session: ProjectSession; chat: ChatController; tab?: WorkflowTab }) {
  const [input, setInput] = useState('')
  const [focused, setFocused] = useState(false)
  const [skillQuery, setSkillQuery] = useState<string | null>(null)
  /** The highlighted Skill candidate (↑ / ↓, Return or Tab takes it). */
  const [skillIndex, setSkillIndex] = useState(0)
  /** The connections set up, each with its chosen model and the models listed in Settings > API. */
  const [connections, setConnections] = useState<{ id: string; name: string; models: string[] }[]>([])
  const [picking, setPicking] = useState(false)
  const [toAttach, setToAttach] = useState<string>()
  const [attaching, setAttaching] = useState(false)
  const [attachError, setAttachError] = useState<string>()
  const inputRef = useRef<HTMLTextAreaElement>(null)

  // Models listed earlier in Settings > API (no request to the provider from here), per connection set up.
  useEffect(() => {
    void Promise.all([
      api<{ provider: string; model: string; openAIModel: string; chatGPTModel: string; models: Record<string, string[] | undefined> }>('/api/settings'),
      api<{ anthropic: boolean; openai: boolean; chatgpt: boolean }>('/api/settings/connections'),
    ]).then(([s, on]) => {
      const list = (id: string, name: string, chosen: string) => ({ id, name, models: [...new Set([...(s.models?.[id] ?? []), chosen])].sort() })
      setConnections([
        ...(on.anthropic ? [list('anthropic', 'Anthropic API', s.model)] : []),
        ...(on.openai ? [list('openai', 'OpenAI API', s.openAIModel)] : []),
        ...(on.chatgpt ? [list('chatgpt', 'ChatGPT プラン', s.chatGPTModel)] : []),
      ])
    }).catch(() => {})
  }, [chat.provider.name, chat.provider.model])

  // Starts one line high and grows with the text (up to a limit).
  useLayoutEffect(() => {
    const el = inputRef.current
    if (!el) return
    el.style.height = 'auto'
    el.style.height = `${Math.min(160, Math.max(22, el.scrollHeight))}px`
    el.style.overflowY = el.scrollHeight > 160 ? 'auto' : 'hidden'
  }, [input])

  const matches: Skill[] = skillQuery === null ? [] : chat.skills.filter((x) => !skillQuery || x.name.toLowerCase().includes(skillQuery.toLowerCase())).slice(0, 8)
  const highlighted = matches.length ? matches[Math.min(skillIndex, matches.length - 1)] : undefined
  const model = chat.provider.model
  const modelName = (m: string) => chat.provider.modelNames?.[m] ?? m
  const cmd = isMac ? '⌘' : 'Ctrl+'

  const send = () => {
    if (!input.trim() || chat.busy || !session.connected) return
    chat.send(input)
    setInput('')
    setSkillQuery(null)
  }

  /** `$` at the start of the word being typed opens the Skill picker. */
  const updateSkillQuery = (text: string) => {
    setSkillIndex(0)
    const lastWord = text.split(' ').pop()?.split('\n').pop() ?? ''
    if (!lastWord.startsWith('$')) return setSkillQuery(null)
    const q = lastWord.slice(1)
    setSkillQuery(chat.skills.some((x) => x.name === q) ? null : q)
  }

  const insertSkill = (sk: Skill) => {
    const token = '$' + (skillQuery ?? '')
    const i = input.lastIndexOf(token)
    setInput(i >= 0 ? input.slice(0, i) + `$${sk.name} ` + input.slice(i + token.length) : input + `$${sk.name} `)
    setSkillQuery(null)
    inputRef.current?.focus()
  }

  /** A model of a connection chosen in the menu: that connection is used from now on. */
  const choose = (provider: string, m: string) => {
    const key = provider === 'chatgpt' ? 'chatGPTModel' : provider === 'openai' ? 'openAIModel' : 'model'
    void api('/api/settings', { provider, [key]: m }).then(() => { void chat.refreshProvider(); void guide.refresh() })
  }

  const onKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.nativeEvent.isComposing) return
    const mod = isMac ? e.metaKey : e.ctrlKey
    if (e.key === 'Enter') {
      if (!mod && !e.shiftKey && highlighted) {
        e.preventDefault()
        return insertSkill(highlighted)
      }
      // ⌘Return always sends; Return too with the standard setting; ⇧Return is a new line.
      if (mod || (prefs.sendKey === 'returnKey' && !e.shiftKey)) {
        e.preventDefault()
        send()
      }
    } else if (e.key === 'Tab' && highlighted) {
      e.preventDefault()
      insertSkill(highlighted)
    } else if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && skillQuery !== null && matches.length) {
      e.preventDefault()
      setSkillIndex((i) => (i + (e.key === 'ArrowUp' ? matches.length - 1 : 1)) % matches.length)
    } else if (e.key === 'Escape' && skillQuery !== null) {
      e.preventDefault()
      e.stopPropagation()
      setSkillQuery(null)
    }
  }

  return (
    <div className="composer">
      {chat.folders.length > 0 && (
        <div className="chip-row">
          {chat.folders.map((f) => (
            <span key={f} className="chip chip-folder" title={f}>
              <Icon name="folder" size={10} />{f.split('/').pop()}
              <Icon name="lock" size={9} className="secondary" title="読み取り専用で共有しています。Agent だけが読みます。" />
              <button type="button" className="chip-x" title="添付を外す" aria-label="添付を外す" onClick={() => chat.detach(f)}><Icon name="xmark.circle.fill" size={11} /></button>
            </span>
          ))}
        </div>
      )}
      {chat.references.length > 0 && (
        <div className="chip-row">
          {chat.references.map((r) => (
            <span key={refLabel(r)} className="chip">
              <Icon name="paperclip" size={10} />{refLabel(r)}
              <button type="button" className="chip-x" title="参照を外す" aria-label="参照を外す" onClick={() => chat.removeReference(r)}><Icon name="xmark.circle.fill" size={11} /></button>
            </span>
          ))}
        </div>
      )}
      {skillQuery !== null && (
        <div className="skill-picker">
          {!matches.length && (
            <div className="caption secondary pad6">{chat.skills.length ? '一致する Skill はありません' : 'Skill がありません（設定 > Skills でフォルダを追加、またはプロジェクトの skills/）'}</div>
          )}
          {matches.map((sk, i) => (
            <button key={sk.path} type="button" className={`skill-item${i === skillIndex ? ' highlighted' : ''}`} onMouseDown={(e) => e.preventDefault()} onClick={() => insertSkill(sk)}>
              <div className="mono callout">${sk.name}</div>
              <div className="caption secondary ellipsis">{sk.description} · {sk.source}</div>
            </button>
          ))}
        </div>
      )}
      <div className={`composer-box${focused ? ' focused' : ''}`}>
        <div className="composer-input">
          {/* Gone while typing (and while an input method composes). */}
          {!input && !focused && <span className="composer-placeholder">メッセージ（{prefs.sendKey === 'returnKey' ? 'Return' : `${cmd}Return`} で送信）</span>}
          <textarea ref={inputRef} className="chat-input" rows={1} value={input} aria-label="メッセージ"
            onFocus={() => setFocused(true)} onBlur={() => setFocused(false)}
            onChange={(e) => { setInput(e.target.value); updateSkillQuery(e.target.value) }}
            onKeyDown={onKeyDown} />
        </div>
        <div className="composer-bar">
          <MenuButton icon={attaching ? 'hourglass' : 'paperclip'} help="参照・フォルダを添付" items={() => [
            { label: '選択した Card を参照に追加', disabled: !tab || tab.selection.size === 0, action: () => { if (tab) for (const k of [...tab.selection].sort()) chat.addReference({ kind: 'card', card: k }) } },
            { label: 'フォルダを添付…', disabled: attaching || !session.connected, action: () => setPicking(true) },
          ]} />
          {/* The model, as quiet text under the input (a menu, not a pop-up box). */}
          <button type="button" className="model-menu" disabled={!chat.provider.remote} title={`モデル（接続先: ${chat.provider.name}）`}
            onClick={(e) => {
              const r = e.currentTarget.getBoundingClientRect()
              // Every connection set up, with its models: choosing one switches to it.
              const current = chat.provider.plan ? 'chatgpt' : chat.provider.api === 'openAIResponses' ? 'openai' : 'anthropic'
              showMenu({ clientX: r.left, clientY: r.bottom + 2 }, connections.flatMap((c, i) => [
                i > 0 ? 'divider' as const : null,
                { label: c.name, disabled: true },
                ...c.models.map((m) => ({ label: modelName(m), checked: c.id === current && m === model, action: () => choose(c.id, m) })),
              ]))
            }}>
            <span className="ellipsis">{modelName(model)}</span>
            <Icon name="chevron.down" size={8} />
          </button>
          <span className="spacer" />
          <span className="caption2 tertiary" title={`直近の入力トークン数（上限に近づくと自動で圧縮）。この会話でキャッシュから読んだ量 ${chat.usage.cacheReadTokens ?? 0}、キャッシュに書いた量 ${chat.usage.cacheWriteTokens ?? 0}`}>{Math.floor(chat.usage.inputTokens / 1000)}k</span>
          {chat.busy
            ? <button type="button" className="send-button" onClick={() => chat.cancel()} title="回答を停止（Workflow の実行は止まりません）" aria-label="回答を停止"><Icon name="stop.circle.fill" size={22} /></button>
            : <button type="button" className="send-button" disabled={!input.trim() || !session.connected || !chat.available} onClick={send}
                title={prefs.sendKey === 'returnKey' ? '送信（Return）' : `送信（${cmd}Return）`} aria-label="送信"><Icon name="arrow.up.circle.fill" size={22} /></button>}
        </div>
      </div>
      {picking && <FolderPicker message="チャットに添付するフォルダ（中身は Agent が読み、接続先に送られます）" onClose={() => setPicking(false)} onPick={(p) => {
        setPicking(false)
        setAttachError(undefined)
        setToAttach(p)
      }} />}
      {toAttach && (
        <Sheet onClose={() => setToAttach(undefined)} width={440}>
          <div className="confirm">
            <div className="confirm-title">フォルダを添付しますか？</div>
            <div className="secondary">「{toAttach.split('/').pop()}」を読み取り専用で Agent が読めるようにします。Agent が読んだファイルの中身は、接続先（{chat.provider.name}）に送られます。</div>
            {attachError && <div className="callout selectable" style={{ color: 'var(--red)' }}>{attachError}</div>}
            <div className="row end gap8">
              <Button onClick={() => setToAttach(undefined)}>キャンセル</Button>
              <Button kind="prominent" disabled={attaching} onClick={async () => {
                setAttaching(true)
                try {
                  await chat.attach(toAttach)
                  setToAttach(undefined)
                } catch (e) {
                  setAttachError(errorText(e))
                } finally {
                  setAttaching(false)
                }
              }}>添付</Button>
            </div>
          </div>
        </Sheet>
      )}
    </div>
  )
}

// ---- Tool steps ----------------------------------------------------------------------

/** A tool call in words: what is being done and on which Card or file. */
export function toolStep(name: string, args: string) {
  let v: any
  try {
    v = JSON.parse(args.split('\n→ ')[0])
  } catch {}
  const segs: string[] | undefined = Array.isArray(v?.card?.segments) ? v.card.segments : Array.isArray(v?.card) ? v.card : undefined
  const card: CardKey | undefined = segs?.length ? key(segs.map(String)) : typeof v?.card === 'string' ? v.card : undefined
  const target: string | undefined = card ?? (typeof v?.path === 'string' ? v.path : typeof v?.definition === 'string' ? v.definition : undefined)
  const n = name.toLowerCase()
  const [running, done] =
    n === 'delegate' ? ['手分けして進めています', '手分けして進めました']
    : n.includes('save') || n.includes('put') || n.includes('write') ? ['変更を保存しています', '変更を保存']
    : n.includes('run') && !n.includes('status') ? ['実行しています', '実行']
    : n.includes('read') || n.includes('inspect') || n.includes('get') ? ['確認しています', '確認']
    : n.includes('log') ? ['ログを読んでいます', 'ログを確認']
    : n.includes('validate') || n.includes('check') ? ['検証しています', '検証']
    : n.includes('format') ? ['整形しています', '整形']
    : n.includes('status') || n.includes('record') || n.includes('history') ? ['結果を確認しています', '結果を確認']
    : [`${name} を実行しています`, name]
  const t = target ? `（${target}）` : ''
  return { card, running: running + t + '…', done: done + t }
}

/** A helper at work (delegate): its name and what it does now, or how its
 *  work ended. */
function HelperRow({ item }: { item: ChatItem }) {
  const step = item.tool ? toolStep(item.tool.name, item.text) : undefined
  const ok = item.tool?.ok
  const text = item.ended ?? (step ? (ok === undefined ? step.running : step.done + (ok ? '' : '：失敗')) : '考えています…')
  return (
    <div className="chat-tool row gap6 center" style={{ paddingLeft: 10 }}>
      <span className="caption" style={{ fontWeight: 600 }}>{item.helper}</span>
      <span className="caption ellipsis" style={{ color: ok === false && !item.ended ? 'var(--red)' : 'var(--secondary)' }}>{text}</span>
      <span className="spacer" />
      {!item.ended && <Spinner small />}
    </div>
  )
}

/** A folded tool step: one line saying what was done; details on demand. */
function ToolRow({ item, session, tab }: { item: ChatItem; session: ProjectSession; tab?: WorkflowTab }) {
  const [open, setOpen] = useState(false)
  const step = toolStep(item.tool?.name ?? '', item.text)
  const ok = item.tool?.ok
  return (
    <div className="chat-tool">
      <div className="row gap6 center">
        <span className="tool-mark">
          {ok === undefined ? <Spinner small /> : <Icon name={ok ? 'checkmark.circle' : 'xmark.circle.fill'} style={{ color: ok ? 'var(--secondary)' : 'var(--red)' }} />}
        </span>
        <span className="caption ellipsis" style={{ color: ok === false ? 'var(--red)' : 'var(--secondary)' }}>
          {ok === undefined ? step.running : step.done + (ok ? '' : '：失敗')}
        </span>
        {step.card && tab && session.instance(step.card) && (
          <button type="button" className="link caption" onClick={() => {
            const k = step.card!
            tab.enter(path(k).slice(0, -1))
            tab.setSelection([k])
            canvasFor(session, tab).reveal(k)
          }}>Card を表示</button>
        )}
        <span className="spacer" />
        <Button kind="borderless" iconOnly small icon={open ? 'chevron.up' : 'chevron.down'} help={open ? '詳細を閉じる' : 'ツールの詳細'} onClick={() => setOpen(!open)} />
      </div>
      {open && <pre className="tool-detail selectable">{item.text.split('\n').slice(0, 40).join('\n')}</pre>}
    </div>
  )
}

// ---- Answers -------------------------------------------------------------------------

/** The answer being written: shown at a steady pace that catches up with
 *  what arrived (no jumps of whole chunks), new blocks fading in. */
function StreamingText({ text }: { text: string }) {
  const [shown, setShown] = useState(0)
  useEffect(() => {
    if (shown >= text.length) return
    // About 0.25 s to catch up with whatever arrived, a few characters per frame at least.
    const t = setTimeout(() => setShown((s) => Math.min(text.length, s + Math.max(2, Math.floor((text.length - s) / 15)))), 16)
    return () => clearTimeout(t)
  }, [shown, text.length])
  return <Markdown text={text.slice(0, shown)} className="selectable chat-assistant" renderCode={codeBlock} fadeIn />
}

/** A finished answer as Markdown with math; very long answers are folded. */
function ChatMarkdown({ text }: { text: string }) {
  const [expanded, setExpanded] = useState(false)
  const long = text.length > 6000 && !expanded
  return (
    <div className="col gap8">
      <Markdown text={long ? fold(text) : text} className="selectable chat-assistant" renderCode={codeBlock} />
      {long && <button type="button" className="link caption chat-more" onClick={() => setExpanded(true)}>続きを表示</button>}
    </div>
  )
}

/** The first part of a long answer, cut between paragraphs (never inside code). */
function fold(text: string): string {
  let out = ''
  let fence = false
  for (const line of text.split('\n')) {
    if (line.startsWith('```')) fence = !fence
    out += line + '\n'
    if (out.length > 4000 && !fence && !line.trim()) break
  }
  return out
}

const codeBlock = (code: string, language: string) => <ChatCodeBlock code={code.replace(/\n$/, '')} language={language} />

function ChatCodeBlock({ code, language }: { code: string; language: string }) {
  const [copied, setCopied] = useState(false)
  const [expanded, setExpanded] = useState(false)
  const lines = code.split('\n')
  const folded = lines.length > 30 && !expanded
  return (
    <div className="chat-code">
      <div className="chat-code-head">
        <span className="caption2 secondary">{language || 'code'}</span>
        <span className="spacer" />
        <button type="button" className="chat-code-copy caption2" onClick={() => void copyText(code).then(() => setCopied(true))}>{copied ? 'コピーしました' : 'コピー'}</button>
      </div>
      <pre className="chat-code-text selectable">{folded ? lines.slice(0, 30).join('\n') : code}</pre>
      {folded && <button type="button" className="link caption chat-code-more" onClick={() => setExpanded(true)}>残り {lines.length - 30} 行を表示</button>}
    </div>
  )
}
