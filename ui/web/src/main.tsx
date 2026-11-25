import { createRoot } from 'react-dom/client'
import '@xterm/xterm/css/xterm.css'
import './styles.css'
import './styles/canvas.css'
import './styles/demoPlayer.css'
import './styles/detail.css'
import './styles/shell.css'
import './styles/chat.css'
import './styles/guide.css'
import { App } from './ui/App'
import { api } from './api'
import { L, adoptLanguage } from './i18n'
import { useState } from 'react'

const startupError = '/api/host/startup-error'
interface StateRecovery { state_format_path: string; recovery_token: string }

function RecoverHost({ issue }: { issue: StateRecovery }) {
  const [pending, setPending] = useState(false)
  const [error, setError] = useState('')
  async function moveAside() {
    if (!window.confirm(L('別名で保存して再作成しますか？') + '\n' + issue.state_format_path)) return
    setPending(true)
    setError('')
    try {
      const response = await fetch(startupError, {
        method: 'POST', headers: { 'X-Tsugumi-Recovery': issue.recovery_token },
      })
      if (!response.ok) throw new Error((await response.json()).error)
      // The recovery listener is replaced by the regular host on this origin.
      for (let attempt = 0; attempt < 60; attempt++) {
        await new Promise((resolve) => setTimeout(resolve, 500))
        const next = await fetch(startupError).catch(() => undefined)
        if (!next) continue
        if (!next.ok || (await next.json()).recovery_token !== issue.recovery_token) {
          location.reload()
          return
        }
      }
      throw new Error(L('実行環境を起動できませんでした'))
    } catch (error) {
      setError(String(error))
      setPending(false)
    }
  }
  return (
    <main className="problem-page scroll">
      <div className="problem-body">
        <h1>{L('状態データの形式が異なります')}</h1>
        <p>{L('この状態データは別のバージョンの Tsugumi で書き込まれました。')}</p>
        <p className="selectable wrap-pre">{issue.state_format_path}</p>
        <p>{L('元のファイルを別名で保存し、新しい状態データを作成します。')}</p>
        <button type="button" disabled={pending} onClick={() => void moveAside()}>{L('別名で保存して再作成…')}</button>
        {error && <p role="alert">{error}</p>}
      </div>
    </main>
  )
}

async function launch() {
  const root = createRoot(document.getElementById('root')!)
  const issue: StateRecovery | undefined = await fetch(startupError).then(
    async (response) => response.ok ? await response.json() : undefined,
  ).catch(() => undefined)
  if (issue?.state_format_path) {
    root.render(<RecoverHost issue={issue} />)
    return
  }
  // The server keeps the language setting shared with the desktop app.
  void api<{ language?: string }>('/api/settings').then((s) => adoptLanguage(s.language), () => {})
  root.render(<App />)
}

void launch()
