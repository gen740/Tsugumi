// A terminal in the open project's folder: a shell on this host (the web UI
// runs the core here, without the VM), on a pseudo terminal, drawn by xterm.js.
import type { FitAddon } from '@xterm/addon-fit'
import type { Terminal } from '@xterm/xterm'
import { useEffect, useRef, useState } from 'react'
import { api, onServerEvent, TerminalEvent } from '../api'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { errorText, ProjectSession } from '../model/session'
import { TerminalInput } from '../model/terminalInput'
import { Button } from './controls'
import { Icon } from './icons'

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

/** Draws a terminal in the element and connects it; returns how to end it. */
function openTerminal(el: HTMLElement, xterm: typeof import('@xterm/xterm'), fitAddon: typeof import('@xterm/addon-fit'),
  session: ProjectSession, onEnd: (m: string) => void, shown: { current?: { term: Terminal; fit: FitAddon } }) {
  const css = getComputedStyle(el)
  const term = new xterm.Terminal({
    fontFamily: css.getPropertyValue('--mono').trim() || 'monospace',
    fontSize: prefs.editorFontSize,
    scrollback: 5000,
    theme: {
      background: css.getPropertyValue('--text-bg').trim(),
      foreground: css.getPropertyValue('--label').trim(),
      cursor: css.getPropertyValue('--label').trim(),
    },
  })
  const fit = new fitAddon.FitAddon()
  term.loadAddon(fit)
  shown.current = { term, fit }
  term.open(el)
  fit.fit()
  term.focus()

  let mine: string | undefined
  let closed = false
  // What it prints before its id is known (the first prompt) waits here.
  const early: TerminalEvent[] = []
  const onEvent = (e: TerminalEvent) => {
    if (e.type === 'terminal') term.write(e.data)
    else if (e.type === 'terminal-exit') {
      term.write('\r\n[ターミナルは終了しました]\r\n')
      onEnd('終了しました')
    }
  }
  const off = onServerEvent((e: TerminalEvent) => {
    if (e.type !== 'terminal' && e.type !== 'terminal-exit') return
    if (!mine) early.push(e)
    else if (e.id === mine) onEvent(e)
  })
  const input = new TerminalInput(
    (data) => api('/api/terminal/input', { id: mine, data }),
    (e) => term.write(`\r\n${errorText(e)}\r\n`),
  )
  term.onData((data) => {
    if (mine) input.write(data)
  })
  term.onResize(({ cols, rows }) => {
    if (mine) void api('/api/terminal/resize', { id: mine, cols, rows }).catch(() => {})
  })
  const seen = new ResizeObserver(() => {
    // Hidden, it has no size to fit.
    if (el.offsetWidth) fit.fit()
  })
  seen.observe(el)
  void (async () => {
    try {
      const r = await api<{ id: string }>('/api/terminal/start', { path: session.dir, cols: term.cols, rows: term.rows })
      if (closed) {
        void api('/api/terminal/stop', { id: r.id }).catch(() => {})
        return
      }
      mine = r.id
      for (const e of early.splice(0)) if (e.id === mine) onEvent(e)
    } catch (e) {
      term.write(`ターミナルを開けませんでした: ${errorText(e)}\r\n`)
      onEnd('開けませんでした')
    }
  })()
  return () => {
    closed = true
    seen.disconnect()
    off()
    if (mine) void api('/api/terminal/stop', { id: mine }).catch(() => {})
    term.dispose()
  }
}

function TerminalHost({ session, onEnd }: { session: ProjectSession; onEnd: (m: string) => void }) {
  const box = useRef<HTMLDivElement>(null)
  const shown = useRef<{ term: Terminal; fit: FitAddon }>()
  useObserve(prefs)
  // The size set in the settings applies at once.
  useEffect(() => {
    const s = shown.current
    if (!s || s.term.options.fontSize === prefs.editorFontSize) return
    s.term.options.fontSize = prefs.editorFontSize
    s.fit.fit()
  }, [prefs.editorFontSize])

  useEffect(() => {
    // Loaded here: xterm.js needs a page as soon as it is imported.
    let gone = false
    let end: (() => void) | undefined
    void Promise.all([import('@xterm/xterm'), import('@xterm/addon-fit')]).then(([xterm, fitAddon]) => {
      if (!gone && box.current) end = openTerminal(box.current, xterm, fitAddon, session, onEnd, shown)
    })
    return () => {
      gone = true
      end?.()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [session.dir])

  return <div className="terminal" ref={box} />
}
