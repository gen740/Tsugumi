// A terminal in the open project's folder: a shell on this host (the web UI
// runs the core here, without the VM), on a pseudo terminal, drawn by xterm.js.
import type { FitAddon } from '@xterm/addon-fit'
import type { Terminal } from '@xterm/xterm'
import { useEffect, useRef, useState } from 'react'
import { openHost, type HostConnection } from '../wire/host'
import * as H from '../../shared/gen/tsugumi/host'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { errorText, ProjectSession } from '../model/session'
import { TerminalInput } from '../model/terminalInput'
import { Button } from './controls'
import { Icon } from './icons'
import { L } from '../i18n'

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
        <span className="caption secondary ellipsis">{L('このホスト上（プロジェクトのフォルダで実行します）')}</span>
        <span className="spacer" />
        {ended && <span className="caption secondary nowrap">{ended}</span>}
        <Button small icon="plus" help={L('ターミナルを開き直します（実行中のコマンドは終了します）')} onClick={() => { setEnded(undefined); setGeneration((g) => g + 1) }}>
          {L('新しいターミナル')}
        </Button>
        {onClose && <Button kind="borderless" small iconOnly icon="xmark" help={L('閉じる（下部のアイコンでも開閉できます）')} onClick={onClose} />}
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

  let client: HostConnection | undefined
  let mine: bigint | undefined
  let closed = false
  let opening = true
  const early: H.HostTerminalDataT[] = []
  const writeError = (e: unknown) => term.write(`\r\n${errorText(e)}\r\n`)
  const onEvent = (e: H.HostTerminalDataT) => {
    if (e.session !== mine) return
    if (e.data.length) term.write(Uint8Array.from(e.data))
    if (e.closed) {
      mine = undefined
      term.write(L('\r\n[ターミナルは終了しました]\r\n'))
      onEnd(L('終了しました'))
    }
  }
  const call = (request: H.HostTerminalRequestT) =>
    client!.call<H.HostTerminalT>(H.RequestPayload.HostTerminalRequest, request, H.ReplyPayload.HostTerminal)
  const encoder = new TextEncoder()
  const input = new TerminalInput(
    (data) => mine === undefined ? Promise.resolve() :
      call(new H.HostTerminalRequestT(null, H.TerminalAction.Input, mine, term.rows, term.cols, [...encoder.encode(data)])),
    writeError,
  )
  term.onData((data) => { if (mine !== undefined) input.write(data) })
  term.onResize(({ cols, rows }) => {
    if (mine !== undefined) void call(new H.HostTerminalRequestT(null, H.TerminalAction.Resize, mine, rows, cols)).catch(writeError)
  })
  const seen = new ResizeObserver(() => {
    // Hidden, it has no size to fit.
    if (el.offsetWidth) fit.fit()
  })
  seen.observe(el)
  void (async () => {
    try {
      const ready = await session.environmentHost.start(session.dir)
      if (closed) return
      if (ready.state !== 'ready') throw new Error(ready.message || L('実行環境が起動していません'))
      client = await openHost()
      if (closed) { client.close(); return }
      client.onClose((reason) => {
        if (closed) return
        mine = undefined
        term.write(`\r\n${reason}\r\n`)
        onEnd(L('終了しました'))
      })
      client.onEvent((event) => {
        if (event.valueType !== H.EventPayload.HostTerminalData) return
        const data = event.value as H.HostTerminalDataT
        if (opening) early.push(data)
        else onEvent(data)
      })
      const r = await call(new H.HostTerminalRequestT(session.dir, H.TerminalAction.Open, 0n, term.rows, term.cols))
      if (closed) { client.close(); return }
      opening = false
      mine = r.session
      for (const e of early.splice(0)) onEvent(e)
    } catch (e) {
      client?.close()
      if (closed) return
      term.write(L('ターミナルを開けませんでした: {0}\r\n', errorText(e)))
      onEnd(L('開けませんでした'))
    }
  })()
  return () => {
    closed = true
    seen.disconnect()
    // hostd can release the session once its Open reply registers ownership.
    if (!opening) client?.close()
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
