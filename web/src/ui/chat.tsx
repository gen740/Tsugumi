// Chat: session list or the conversation (answers, progress, input, model
// and send/stop). Usable docked beside the canvas or in its own window.
import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { api } from '../api'
import { app } from '../model/app'
import { canvasFor } from '../model/canvas'
import { ModelListRefresher } from '../model/modelLists'
import { ChatController, ChatItem, refLabel, SearchHit, Skill, USAGE_URL } from '../model/chat'
import { chatFollowsAfterScroll } from '../model/chatFollow'
import { birdColor } from '../model/helperBirds'
import { useObserve } from '../model/observable'
import { PERMISSIONS, parsePermission, permissionInfo } from '../model/permissions'
import { isMac, prefs } from '../model/prefs'
import { CardKey, commentChange, errorText, key, path, ProjectSession } from '../model/session'
import { AGENT, modelOptions, savedComments } from '../shared/agentShared'
import { WorkflowTab } from '../model/workspace'
import { Button, copyText, Empty, MenuButton, Sheet, showMenu, Spinner, TextField, Toggle } from './controls'
import { Icon } from './icons'
import { Markdown } from './markdown'
import { EvidenceContext, evidenceLink } from './evidence'
import { ChatChoices, GuideNest } from './guide'
import { guide } from '../model/guide'
import { FolderPicker } from './sidebar'
import { L } from '../i18n'

export function ChatPanel({ session, chat, detached, tab }: { session: ProjectSession; chat: ChatController; detached: boolean; tab?: WorkflowTab }) {
  useObserve(chat, session, tab, prefs)
  const [showList, setShowList] = useState(false)

  return (
    <EvidenceContext.Provider value={{ session, tab }}><div className="chat-panel">
      <div className="chat-header">
        {!detached && <GuideNest busy={chat.busy} />}
        <Button kind="borderless" iconOnly icon={showList ? 'bubble.left.and.bubble.right.fill' : 'list.bullet'} help={showList ? L('会話に戻る') : L('会話の一覧と検索')} onClick={() => setShowList(!showList)} />
        <b className="ellipsis flex1">{showList ? L('会話') : chat.sessions.find((x) => x.id === chat.currentId)?.title ?? L('新しい会話')}</b>
        <Button kind="borderless" iconOnly icon="square.and.pencil" help={L('新しい会話')} onClick={() => { chat.newSession(); setShowList(false) }} />
        <MenuButton icon="ellipsis.circle" help={L('その他')} items={() => [
          // Only for the conversation that is open, never from the list.
          !showList && { label: L('会話を圧縮'), disabled: chat.busy || chat.items.length < 6, action: () => chat.compact() },
          !showList && 'divider',
          { label: L('Skill を再読込'), action: () => void chat.reloadSkills() },
          !detached && { label: L('別ウインドウで開く'), action: () => app.openWindow(session, { kind: 'chat' }) },
          'divider',
          { label: L('接続設定…'), action: () => app.openWindow(undefined, { kind: 'appSettings' }) },
        ]} />
      </div>
      {showList ? <SessionList chat={chat} onPick={() => setShowList(false)} /> : <Conversation session={session} chat={chat} tab={tab} />}
    </div></EvidenceContext.Provider>
  )
}

/** Why Fast mode cannot be chosen for the model in use. */
function fastUnavailable(connection: string): string {
  return connection === 'chatgpt' ? L('Fast mode: ChatGPT プランでは選べません（指定が拒否されることがあるため）')
    : connection === 'openai' ? L('Fast mode: このモデルでは選べません（公式の料金表で確かめたモデルだけ選べます）')
    : L('Fast mode: Claude Opus 5.5 でだけ選べます')
}

/** The Agent waits for this answer, so it shows whether or not the chat is open. */
export function PermissionHost({ session }: { session: ProjectSession }) {
  const chat = app.chat(session)
  useObserve(chat)
  const p = chat.permission
  return p ? <PermissionPrompt key={p.id} asked={p.asked} from={p.from} onAnswer={(allow, always) => chat.answerPermission(allow, always)} /> : null
}

/** Presents core-owned access and operation requests. Operations have no
 *  standing approval option. Return allows and Esc declines. */
export function PermissionPrompt({ asked, from, reload = false, onAnswer }: { asked: string[]; from?: string; reload?: boolean; onAnswer: (allow: boolean, always: string[]) => void }) {
  const who = from ?? 'Agent'
  const [always, setAlways] = useState<string[]>([])
  useEffect(() => {
    const key = (e: KeyboardEvent) => {
      // A button with the focus answers for itself.
      if (e.key !== 'Enter' || e.isComposing || (e.target as HTMLElement | null)?.tagName === 'BUTTON') return
      e.preventDefault()
      onAnswer(true, always)
    }
    window.addEventListener('keydown', key, true)
    return () => window.removeEventListener('keydown', key, true)
  }, [onAnswer, always])
  const requests = asked.map(parsePermission).filter((r) => r !== undefined)
  const operation = asked.find((r) => r.includes('operation_approval\n'))?.split('operation_approval\n').slice(1).join('operation_approval\n')
  const access = asked.filter((r) => r.startsWith('project_access '))
  const kinds = operation || reload ? [] : PERMISSIONS.filter((p) => requests.some((r) => r.kind === p.kind))
  return (
    <Sheet width={460} onClose={() => onAnswer(false, [])} backdropCloses={false}>
      <div className="sheet-body permission-prompt">
        <div className="title2"><Icon name="hand.raised" /> {reload ? L('外部の変更が Card の権限を広げます') : operation ? L('{0} がこの操作の承認を求めています', who) : access.length ? L('{0} がアクセスの許可を求めています', who) : L('{0} が Card の権限を広げようとしています', who)}</div>
        <p className="secondary">{reload ? L('表示された権限を許可して、外部の変更を読み込みますか？') : operation ? L('表示された対象への、この操作だけを承認します。次の操作には再び承認が必要です。拒否すると何も変更しません。') : access.length ? L('表示された対象へのアクセスを許可します。読み取った内容は接続先のモデルに送信される場合があります。') : L('許可すると、この保存が行われます。許可しなければ何も保存されず、{0} には理由を説明するよう伝えます。', who)}</p>
        <PermissionRequests asked={asked} />
        {!!kinds.length && <div className="secondary small">{L('今後は確認しない（設定の「API」でいつでも変えられます）')}</div>}
        {kinds.map((k) => (
          <Toggle key={k.kind} kind="checkbox" label={k.label} checked={always.includes(k.kind)}
            onChange={(on) => setAlways(on ? [...always, k.kind] : always.filter((x) => x !== k.kind))} />
        ))}
        <div className="row end gap8">
          <Button onClick={() => onAnswer(false, [])}>{L('許可しない')}</Button>
          <Button kind="prominent" onClick={() => onAnswer(true, always)}>{L('許可する')}</Button>
        </div>
      </div>
    </Sheet>
  )
}

/** The exact widenings listed before approval, shared by reload and Agent saves. */
export function PermissionRequests({ asked }: { asked: string[] }) {
  const requests = asked.map(parsePermission).filter(r => r !== undefined)
  const operation = asked.find(r => r.includes('operation_approval\n'))?.split('operation_approval\n').slice(1).join('operation_approval\n')
  const access = asked.filter(r => r.startsWith('project_access '))
  return (
        <ul className="permission-list">
          {operation && <li><pre style={{ whiteSpace: 'pre-wrap', overflowWrap: 'anywhere', maxHeight: 360, overflow: 'auto' }}>{operation.split('\n').map((line) => {
            try { return JSON.stringify(JSON.parse(line), null, 2) } catch { return line }
          }).join('\n')}</pre></li>}
          {!operation && access.map((r) => <li key={r}><Icon name="hand.raised" /><div>{r.slice('project_access '.length)}</div></li>)}
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
      <div className="sidebar-pad"><TextField value={query} onChange={setQuery} placeholder={L('履歴を検索')} /></div>
      <div className="list flex1">
        {!query && chat.sessions.map((x) => (
          <div key={x.id} className="list-row clickable col-start" onClick={() => open(x.id)}
            onContextMenu={(e) => showMenu(e, [{ label: L('削除'), destructive: true, action: () => void chat.delete(x.id) }])}>
            <div className="ellipsis" style={{ fontWeight: x.id === chat.currentId ? 600 : 400 }}>{x.title}</div>
            <div className="caption secondary">{new Date(x.updatedAt).toLocaleString('ja-JP', { dateStyle: 'medium', timeStyle: 'short' })} · {x.messages} 件</div>
          </div>
        ))}
        {!query && !chat.sessions.length && <Empty title={L('会話はまだありません')} icon="bubble.left" />}
        {query && hits.map((h, i) => (
          <div key={i} className="list-row clickable col-start" onClick={() => open(h.session)}>
            <div className="caption" style={{ fontWeight: 600 }}>{h.title}</div>
            <div className="caption secondary clamp3">{h.snippet}</div>
          </div>
        ))}
        {query && !hits.length && <Empty title={L('“{0}” の結果はありません', query)} icon="magnifyingglass" description={L('スペルを確認するか、新しく検索してください。')} />}
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
  const column = useRef<HTMLDivElement>(null)
  // Opening a conversation or sending a message goes to the end, and the end is
  // kept from then on, until the reader scrolls up.
  const following = useRef(true)
  const lastTop = useRef(0)
  const toEnd = () => {
    const el = scroll.current
    if (!el) return
    el.scrollTop = el.scrollHeight
    lastTop.current = el.scrollTop
  }
  const restart = () => {
    following.current = true
    toEnd()
  }
  useLayoutEffect(restart, [chat.currentId])
  const sent = last?.role === 'user' ? last.id : undefined
  useLayoutEffect(() => {
    if (sent) restart()
  }, [sent])
  // As the conversation changes (also where nothing is being drawn, as in a
  // window behind the others, where the observer below waits).
  useLayoutEffect(() => {
    if (following.current) toEnd()
  }, [chat.items, chat.busy])
  // Any other change of height: Markdown and math laid out or their fonts
  // arriving, an expanded row, the composer, the panel's width.
  useLayoutEffect(() => {
    const el = scroll.current, inner = column.current
    if (!el || !inner) return
    const resized = new ResizeObserver(() => {
      if (following.current) toEnd()
    })
    resized.observe(el)
    resized.observe(inner)
    const scrolled = () => {
      following.current = chatFollowsAfterScroll(following.current, lastTop.current, { top: el.scrollTop, height: el.scrollHeight, viewport: el.clientHeight })
      lastTop.current = el.scrollTop
    }
    el.addEventListener('scroll', scrolled, { passive: true })
    return () => {
      resized.disconnect()
      el.removeEventListener('scroll', scrolled)
    }
  }, [])

  return (
    <>
      <div className="chat-scroll" ref={scroll}>
        <div className="chat-column" ref={column}>
          {chat.items.length === 0 && (
            <div className="chat-intro">
              <div className="callout">{guide.connected ? L('ツグミです。Workflow の相談・生成・編集も、使い方の質問もどうぞ。') : L('Workflow の相談・生成・編集を依頼できます。')}</div>
              <div className="caption secondary">{L('Card を右クリックして参照に追加、$ で Skill を選べます。')}</div>
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
            : chat.contextFull ? <Button small kind="prominent" onClick={() => chat.newSession()}>{L('新しい会話')}</Button>
            : chat.failed && <Button small onClick={() => chat.retry()}>{L('再試行')}</Button>}
          {chat.interrupted && !chat.busy && <Button small onClick={() => chat.resume()}>{L('再開')}</Button>}
          {chat.stoppedAtLimit && !chat.busy && <Button small onClick={() => chat.continueWork()}>{L('続ける')}</Button>}
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
  if (items[items.length - 1]?.role === 'assistant') return L('回答を書いています…')
  return L('考えています…')
}

function Row({ item, streaming, session, tab }: { item: ChatItem; streaming: boolean; session: ProjectSession; tab?: WorkflowTab }) {
  const op = item.archived ? 0.6 : 1
  switch (item.role) {
    case 'user':
      return <div className="chat-user-row" style={{ opacity: op }}><div className="chat-user selectable">{item.text}</div></div>
    case 'tool':
      return <div style={{ opacity: op }}><ToolRow item={item} session={session} tab={tab} /></div>
    case 'helper':
      return <HelperRow item={item} color={birdColor(app.chat(session).team.tint(item.helper ?? ''), prefs.dark)} />
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
  /** The connections set up, each with its chosen model and its saved models. */
  const [connections, setConnections] = useState<{ id: string; name: string; models: string[] }[]>([])
  const [picking, setPicking] = useState(false)
  const [toAttach, setToAttach] = useState<string[]>()
  const [attaching, setAttaching] = useState(false)
  const [attachError, setAttachError] = useState<string>()
  const [askThoughts, setAskThoughts] = useState(false)
  const inputRef = useRef<HTMLTextAreaElement>(null)

  // The saved models per connection set up (no request to the provider from here).
  const loadConnections = () => Promise.all([
    api<{ provider: string; model: string; openAIModel: string; chatGPTModel: string; models: Record<string, string[] | undefined> }>('/api/settings'),
    api<{ anthropic: boolean; openai: boolean; chatgpt: boolean }>('/api/settings/connections'),
  ]).then(([s, on]) => {
    const list = (id: string, name: string, chosen: string) => ({ id, name, models: [...new Set([...(s.models?.[id] ?? []), chosen])].sort() })
    const all = [
      ...(on.anthropic ? [list('anthropic', 'Anthropic API', s.model)] : []),
      ...(on.openai ? [list('openai', 'OpenAI API', s.openAIModel)] : []),
      ...(on.chatgpt ? [list('chatgpt', L('ChatGPT プラン'), s.chatGPTModel)] : []),
    ]
    setConnections(all)
    return all
  })
  useEffect(() => {
    void loadConnections().catch(() => {})
  }, [chat.provider.name, chat.provider.model])
  const modelLists = useRef(new ModelListRefresher((id) => api('/api/settings/models', { provider: id })))

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
    if (!input.trim() || chat.busy || attaching || !session.connected) return
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

  /** The model menu's effort or Fast mode (from the next request on). */
  const chooseOption = (setting: { effort: string } | { fastModel: string }) => {
    void api('/api/settings', setting).then(() => { void chat.refreshProvider() })
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
              <Icon name="folder" size={10} />{chat.folders.filter(other => other.split('/').pop() === f.split('/').pop()).length > 1 ? f : f.split('/').pop()}
              <Icon name="lock" size={9} className="secondary" title={L('読み取り専用で共有しています。Agent だけが読みます。')} />
              <button type="button" className="chip-x" disabled={attaching} title={L('添付を外す')} aria-label={L('添付を外す')} onClick={() => chat.detach(f)}><Icon name="xmark.circle.fill" size={11} /></button>
            </span>
          ))}
        </div>
      )}
      {chat.references.length > 0 && (
        <div className="chip-row">
          {chat.references.map((r) => (
            <span key={refLabel(r)} className="chip">
              <Icon name="paperclip" size={10} />{refLabel(r, k => session.displayPath(path(k)))}
              <button type="button" className="chip-x" title={L('参照を外す')} aria-label={L('参照を外す')} onClick={() => chat.removeReference(r)}><Icon name="xmark.circle.fill" size={11} /></button>
            </span>
          ))}
        </div>
      )}
      {skillQuery !== null && (
        <div className="skill-picker">
          {!matches.length && (
            <div className="caption secondary pad6">{chat.skills.length ? L('一致する Skill はありません') : L('Skill がありません（設定 > Skills でフォルダを追加、またはプロジェクトの skills/）')}</div>
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
          {!input && !focused && <span className="composer-placeholder">{L('メッセージ（{0} で送信）', prefs.sendKey === 'returnKey' ? 'Return' : `${cmd}Return`)}</span>}
          <textarea ref={inputRef} className="chat-input" rows={1} value={input} aria-label={L('メッセージ')}
            onFocus={() => setFocused(true)} onBlur={() => setFocused(false)}
            onChange={(e) => { setInput(e.target.value); updateSkillQuery(e.target.value) }}
            onKeyDown={onKeyDown} />
        </div>
        <div className="composer-bar">
          <MenuButton icon={attaching ? 'hourglass' : 'paperclip'} help={L('参照・フォルダを添付')} items={() => [
            { label: L('選択した Card を参照に追加'), disabled: !tab || tab.selection.size === 0, action: () => { if (tab) for (const k of [...tab.selection].sort()) chat.addReference({ kind: 'card', card: k }) } },
            { label: L('フォルダを添付…'), disabled: attaching || !session.connected, action: () => setPicking(true) },
          ]} />
          {/* The model, as quiet text under the input (a menu, not a pop-up box). */}
          <button type="button" className="model-menu" disabled={!chat.provider.remote} title={L('モデル（接続先: {0}）', chat.provider.name)}
            onClick={(e) => {
              const r = e.currentTarget.getBoundingClientRect()
              // Every connection set up, with its models: choosing one switches to it.
              const items = (list: typeof connections) => {
                const current = chat.provider.plan ? 'chatgpt' : chat.provider.api === 'openAIResponses' ? 'openai' : 'anthropic'
                const options = modelOptions(current, model)
                const { effort, fast } = chat.provider
                return [
                  { label: 'Effort', disabled: true },
                  ...(options.efforts.length
                    ? [{ label: options.defaultEffort ? L('既定（{0}）', options.defaultEffort) : L('既定'), checked: !effort, action: () => chooseOption({ effort: '' }) },
                       ...AGENT.modelOptions.efforts.filter((l) => options.efforts.includes(l)).map((l) => ({ label: l, checked: l === effort, action: () => chooseOption({ effort: l }) }))]
                    : [{ label: L('このモデルでは選べません（公式の文書で対応を確かめたモデルだけ選べます）'), disabled: true }]),
                  'divider' as const,
                  options.fastPrice === undefined
                    ? { label: fastUnavailable(current), disabled: true }
                    : { label: L('Fast mode（料金は通常の {0} 倍）', options.fastPrice), checked: !!fast, action: () => chooseOption({ fastModel: fast ? '' : model }) },
                  ...list.flatMap((c) => [
                    'divider' as const,
                    { label: c.name, disabled: true },
                    ...c.models.map((m) => ({ label: modelName(m), checked: c.id === current && m === chat.provider.model, action: () => choose(c.id, m) })),
                  ]),
                ]
              }
              // The saved lists at once; each connection's new list when it comes.
              const menu = showMenu({ clientX: r.left, clientY: r.bottom + 2 }, items(connections))
              const show = () => loadConnections().then((list) => {
                menu.update(items(list))
                return list
              })
              void show().catch(() => connections).then((list) => modelLists.current.refresh(list.map((c) => c.id), (id, ok) => {
                if (ok) void (id === 'chatgpt' ? chat.refreshProvider() : Promise.resolve()).then(show).catch(() => {})
              }))
            }}>
            <span className="ellipsis">{[modelName(model), chat.provider.effort, chat.provider.fast && 'Fast'].filter(Boolean).join(' · ')}</span>
            <Icon name="chevron.down" size={8} />
          </button>
          <button type="button" className="model-menu" disabled={!chat.provider.thoughtModel}
            title={chat.thoughts.label} aria-label={chat.thoughts.label} aria-pressed={chat.thoughts.enabled}
            onClick={() => { if (!chat.thoughts.toggle()) setAskThoughts(true) }}>
            <Icon name={chat.thoughts.enabled ? 'bubble.left.and.text.bubble.right.fill' : 'bubble.left.and.text.bubble.right'} />
            {chat.thoughts.enabled && <span className="caption2">{L('入力 {0}、出力 {1}', chat.thoughts.usage.inputTokens, chat.thoughts.usage.outputTokens)}</span>}
          </button>
          <span className="spacer" />
          <span className="caption2 tertiary" title={L('直近の入力トークン数（上限に近づくと自動で圧縮）。この会話でキャッシュから読んだ量 {0}、キャッシュに書いた量 {1}', chat.usage.cacheReadTokens ?? 0, chat.usage.cacheWriteTokens ?? 0)}>{Math.floor(chat.usage.inputTokens / 1000)}k</span>
          {chat.busy
            ? <button type="button" className="send-button" onClick={() => chat.cancel()} title={L('回答を停止（Workflow の実行は止まりません）')} aria-label={L('回答を停止')}><Icon name="stop.circle.fill" size={22} /></button>
            : <button type="button" className="send-button" disabled={!input.trim() || attaching || !session.connected || !chat.available} onClick={send}
                title={prefs.sendKey === 'returnKey' ? L('送信（Return）') : L('送信（{0}Return）', cmd)} aria-label={L('送信')}><Icon name="arrow.up.circle.fill" size={22} /></button>}
        </div>
      </div>
      {askThoughts && <Sheet onClose={() => setAskThoughts(false)} width={520}>
        <div className="confirm">
          <div className="confirm-title">{L('思考の過程を描きますか？')}</div>
          <div className="secondary">{L('Agent の発言とツールの結果を、数秒ごとに小さなモデル（{0}、接続先: {1}）に送って要約し、Canvas の鶫の吹き出しに出します。Agent とは別に token を使います。オフにすると、吹き出しはツール呼び出しから作る短い文だけになります。', chat.provider.thoughtModel ?? '', chat.provider.name)}</div>
          <div className="row end gap8">
            <Button onClick={() => setAskThoughts(false)}>{L('キャンセル')}</Button>
            <Button kind="prominent" disabled={!chat.provider.thoughtModel} onClick={() => { chat.thoughts.accept(); setAskThoughts(false) }}>{L('描く')}</Button>
          </div>
        </div>
      </Sheet>}
      {picking && <FolderPicker message={L('チャットに添付するフォルダ（中身は Agent が読み、接続先に送られます）')} onClose={() => setPicking(false)} onPickMultiple={(p) => {
        setPicking(false)
        setAttachError(undefined)
        setToAttach(p)
      }} />}
      {toAttach && (
        <Sheet onClose={() => { if (!attaching) setToAttach(undefined) }} width={520}>
          <div className="confirm">
            <div className="confirm-title">{L('フォルダを添付しますか？')}</div>
            <div className="secondary">{L('「{0}」を読み取り専用で Agent が読めるようにします。Agent が読んだファイルの中身は、接続先（{1}）に送られます。', toAttach.join(', '), chat.provider.name)}</div>
            {attachError && <div className="callout selectable" style={{ color: 'var(--red)' }}>{attachError}</div>}
            <div className="row end gap8">
              <Button disabled={attaching} onClick={() => setToAttach(undefined)}>{L('キャンセル')}</Button>
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
              }}>{L('添付')}</Button>
            </div>
          </div>
        </Sheet>
      )}
    </div>
  )
}

// ---- Tool steps ----------------------------------------------------------------------

/** A tool call in words: what is being done and on which Card or file. */
export function toolStep(name: string, args: string, display = (k: CardKey) => k) {
  let v: any
  try {
    v = JSON.parse(args.split('\n→ ')[0])
  } catch {}
  const segs: string[] | undefined = Array.isArray(v?.card?.segments) ? v.card.segments : Array.isArray(v?.card) ? v.card : undefined
  const card: CardKey | undefined = segs?.length ? key(segs.map(String)) : typeof v?.card === 'string' ? v.card : undefined
  const target: string | undefined = card ?? (typeof v?.path === 'string' ? v.path : typeof v?.definition === 'string' ? v.definition : typeof v?.query === 'string' ? v.query : undefined)
  const n = name.toLowerCase()
  const [running, done] =
    n === 'delegate' ? [L('手分けして進めています'), L('手分けして進めました')]
    : n === 'web_search' ? [L('Web を検索しています'), L('Web を検索')]
    : n.includes('save') || n.includes('put') || n.includes('write') ? [L('変更を保存しています'), L('変更を保存')]
    : n.includes('run') && !n.includes('status') ? [L('実行しています'), L('実行')]
    : n.includes('read') || n.includes('inspect') || n.includes('get') ? [L('確認しています'), L('確認')]
    : n.includes('log') ? [L('ログを読んでいます'), L('ログを確認')]
    : n.includes('validate') || n.includes('check') ? [L('検証しています'), L('検証')]
    : n.includes('format') ? [L('整形しています'), L('整形')]
    : n.includes('status') || n.includes('record') || n.includes('history') ? [L('結果を確認しています'), L('結果を確認')]
    : [L('{0} を実行しています', name), name]
  const t = target ? L('（{0}）', card ? display(card) : target) : ''
  return { card, running: running + t + '…', done: done + t }
}

/** A helper at work (delegate): its name and what it does now, or how its
 *  work ended. */
function HelperRow({ item, color }: { item: ChatItem; color: string }) {
  const step = item.tool ? toolStep(item.tool.name, item.text) : undefined
  const ok = item.tool?.ok
  const text = item.ended ?? (step ? (ok === undefined ? step.running : step.done + (ok ? '' : L('：失敗'))) : L('考えています…'))
  return (
    <div className="helper-row">
      <span className="helper-dot" style={{ background: color }} />
      <span className="caption" style={{ fontWeight: 600, color }}>{item.helper}</span>
      <span className="caption ellipsis" style={{ color: ok === false && !item.ended ? 'var(--red)' : 'var(--secondary)' }}>{text}</span>
      <span className="spacer" />
      {!item.ended && <Spinner small />}
    </div>
  )
}

/** A search's details with its sources as links (only http and https). */
function Linked({ text }: { text: string }) {
  return <>{text.split(/(https?:\/\/[^\s]+)/).map((part, i) =>
    (i % 2 ? <a key={i} href={part} target="_blank" rel="noreferrer noopener">{part}</a> : part))}</>
}

/** A folded tool step: one line saying what was done; details on demand. */
function ToolRow({ item, session, tab }: { item: ChatItem; session: ProjectSession; tab?: WorkflowTab }) {
  const [open, setOpen] = useState(false)
  const step = toolStep(item.tool?.name ?? '', item.text, k => session.displayPath(path(k)))
  const ok = item.tool?.ok
  return (
    <div className="chat-tool">
      <div className="row gap6 center">
        <span className="tool-mark">
          {ok === undefined ? <Spinner small /> : <Icon name={ok ? 'checkmark.circle' : 'xmark.circle.fill'} style={{ color: ok ? 'var(--secondary)' : 'var(--red)' }} />}
        </span>
        <span className="caption ellipsis" style={{ color: ok === false ? 'var(--red)' : 'var(--secondary)' }}>
          {ok === undefined ? step.running : step.done + (ok ? '' : L('：失敗'))}
        </span>
        {step.card && tab && session.instance(step.card) && (
          <button type="button" className="link caption" onClick={() => {
            const k = step.card!
            tab.enter(path(k).slice(0, -1))
            tab.setSelection([k])
            canvasFor(session, tab).reveal(k)
          }}>{L('Card を表示')}</button>
        )}
        <span className="spacer" />
        <Button kind="borderless" iconOnly small icon={open ? 'chevron.up' : 'chevron.down'} help={open ? L('詳細を閉じる') : L('ツールの詳細')} onClick={() => setOpen(!open)} />
      </div>
      {savedComments(item.text).map((c, i) => (
        <div key={i} className="chat-tool-comment caption secondary selectable">
          {L('{0} · コメントを{1}: {2}', session.displayPath(path(c.card)), commentChange(c.change, c.by), c.text)}
          {c.resolution && ` — ${c.resolution}`}
          {!!c.cards?.length && L('（変更した Card: {0}）', c.cards.map(k => session.displayPath(path(k))).join('、'))}
        </div>
      ))}
      {open && <pre className="tool-detail selectable">{item.tool?.name === 'web_search' ? <Linked text={item.text} /> : item.text.split('\n').slice(0, 40).join('\n')}</pre>}
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
  return <Markdown text={text.slice(0, shown)} className="selectable chat-assistant" renderCode={codeBlock} renderLink={evidenceLink} fadeIn />
}

/** A finished answer as Markdown with math; very long answers are folded. */
function ChatMarkdown({ text }: { text: string }) {
  const [expanded, setExpanded] = useState(false)
  const long = text.length > 6000 && !expanded
  return (
    <div className="col gap8">
      <Markdown text={long ? fold(text) : text} className="selectable chat-assistant" renderCode={codeBlock} renderLink={evidenceLink} />
      {long && <button type="button" className="link caption chat-more" onClick={() => setExpanded(true)}>{L('続きを表示')}</button>}
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
        <button type="button" className="chat-code-copy caption2" onClick={() => void copyText(code).then(() => setCopied(true))}>{copied ? L('コピーしました') : L('コピー')}</button>
      </div>
      <pre className="chat-code-text selectable">{folded ? lines.slice(0, 30).join('\n') : code}</pre>
      {folded && <button type="button" className="link caption chat-code-more" onClick={() => setExpanded(true)}>{L('残り {0} 行を表示', lines.length - 30)}</button>}
    </div>
  )
}
