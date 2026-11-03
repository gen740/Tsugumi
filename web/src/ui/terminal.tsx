// A terminal in the open project's folder: a shell on this host (the web UI
// runs the core here, without the VM). There is no pseudo terminal, so
// commands are sent a line at a time and full-screen programs do not work.
import { useEffect, useRef, useState } from 'react'
import { api, onServerEvent, TerminalEvent } from '../api'
import { useObserve } from '../model/observable'
import { isMac, prefs } from '../model/prefs'
import { errorText, ProjectSession } from '../model/session'
import { Button } from './controls'
import { Icon } from './icons'

const MAX_OUTPUT = 512 * 1024
// Colors and cursor movement of programs that ignore TERM=dumb.
// eslint-disable-next-line no-control-regex
const ANSI = /\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[()][A-Za-z0-9]|\r(?!\n)/g

export function ProjectTerminalPanel({ session, onClose }: { session: ProjectSession; onClose?: () => void }) {
  useObserve(session)
  const [generation, setGeneration] = useState(0)
  const [ended, setEnded] = useState<string>()
  return (
    <div className="terminal-panel">
      <div className="panel-bar">
        <Icon name="terminal" />
        <b className="ellipsis">{session.folderName}</b>
        <span className="callout mono secondary ellipsis" title={session.dir}>{session.dir}</span>
        <span className="caption secondary ellipsis">このホスト上（プロジェクトのフォルダで実行します）</span>
        <span className="spacer" />
        {ended && <span className="caption secondary nowrap">{ended}</span>}
        <Button small icon="plus" help="ターミナルを開き直します（実行中のコマンドは終了します）" onClick={() => { setEnded(undefined); setGeneration((g) => g + 1) }}>
          新しいターミナル
        </Button>
        {onClose && <Button kind="borderless" small iconOnly icon="xmark" help="閉じる（下部のアイコンでも開閉できます）" onClick={onClose} />}
      </div>
      <TerminalHost key={generation} session={session} onEnd={setEnded} />
    </div>
  )
}

function TerminalHost({ session, onEnd }: { session: ProjectSession; onEnd: (m: string) => void }) {
  const [out, setOut] = useState('')
  const [line, setLine] = useState('')
  const [id, setId] = useState<string>()
  const [alive, setAlive] = useState(false)
  const history = useRef<{ list: string[]; at: number }>({ list: [], at: 0 })
  const scroller = useRef<HTMLDivElement>(null)
  const input = useRef<HTMLInputElement>(null)
  const follow = useRef(true)

  const feed = (t: string) => setOut((o) => {
    const n = o + t.replace(ANSI, '')
    return n.length > MAX_OUTPUT ? n.slice(n.length - MAX_OUTPUT) : n
  })

  useEffect(() => {
    let mine: string | undefined
    let closed = false
    const off = onServerEvent((e: TerminalEvent) => {
      if (!mine || e.id !== mine) return
      if (e.type === 'terminal') feed(e.data)
      else if (e.type === 'terminal-exit') {
        feed('\n[ターミナルは終了しました]\n')
        setAlive(false)
        onEnd('終了しました')
      }
    })
    void (async () => {
      try {
        const r = await api<{ id: string; cwd: string; shell: string }>('/api/terminal/start', { path: session.dir })
        if (closed) {
          void api('/api/terminal/stop', { id: r.id }).catch(() => {})
          return
        }
        mine = r.id
        setId(r.id)
        setAlive(true)
        feed(`${r.shell} — ${r.cwd}\n`)
      } catch (e) {
        feed(`\nターミナルを開けませんでした: ${errorText(e)}\n`)
        onEnd('開けませんでした')
      }
    })()
    return () => {
      closed = true
      off()
      if (mine) void api('/api/terminal/stop', { id: mine }).catch(() => {})
    }
  }, [session.dir])

  useEffect(() => {
    const el = scroller.current
    if (el && follow.current) el.scrollTop = el.scrollHeight
  }, [out])

  const send = (text: string) => {
    if (!id || !alive) return
    void api('/api/terminal/input', { id, data: text }).catch((e) => feed(`\n${errorText(e)}\n`))
  }

  const keyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    const h = history.current
    if (e.key === 'Enter' && !e.nativeEvent.isComposing) {
      e.preventDefault()
      feed(`$ ${line}\n`)
      send(line + '\n')
      if (line.trim() && h.list[h.list.length - 1] !== line) h.list.push(line)
      h.at = h.list.length
      setLine('')
      follow.current = true
    } else if (e.key === 'ArrowUp' && h.list.length) {
      e.preventDefault()
      h.at = Math.max(0, h.at - 1)
      setLine(h.list[h.at] ?? '')
    } else if (e.key === 'ArrowDown' && h.list.length) {
      e.preventDefault()
      h.at = Math.min(h.list.length, h.at + 1)
      setLine(h.list[h.at] ?? '')
    } else if (e.ctrlKey && !e.metaKey && e.key === 'c' && (isMac || input.current?.selectionStart === input.current?.selectionEnd)) {
      e.preventDefault()
      feed(`$ ${line}^C\n`)
      setLine('')
      if (id && alive) void api('/api/terminal/interrupt', { id }).catch(() => {})
    } else if (e.ctrlKey && !e.metaKey && e.key === 'l') {
      e.preventDefault()
      setOut('')
    } else if (e.ctrlKey && !e.metaKey && e.key === 'd' && !line) {
      e.preventDefault()
      if (id && alive) void api('/api/terminal/stop', { id }).catch(() => {})
    }
  }

  return (
    <div className="terminal" onMouseUp={() => { if (!window.getSelection()?.toString()) input.current?.focus() }}>
      <div className="terminal-out" ref={scroller}
        onScroll={(e) => {
          const el = e.currentTarget
          follow.current = el.scrollHeight - el.scrollTop - el.clientHeight < 24
        }}>
        <pre className="selectable" style={{ fontSize: prefs.editorFontSize }}>{out}</pre>
      </div>
      <form className="terminal-line" onSubmit={(e) => e.preventDefault()}>
        <span className="mono secondary">$</span>
        <input ref={input} className="terminal-input mono" value={line} disabled={!alive} autoFocus spellCheck={false} autoComplete="off" autoCapitalize="off"
          aria-label="コマンド" placeholder={alive ? 'コマンドを入力して Return（⌃C で中断、⌃L で消去）' : ''}
          style={{ fontSize: prefs.editorFontSize }} onChange={(e) => setLine(e.target.value)} onKeyDown={keyDown} />
      </form>
    </div>
  )
}
