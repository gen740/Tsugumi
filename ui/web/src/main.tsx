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
import { adoptLanguage } from './i18n'

// The language is the server's setting (shared with the desktop app on a Mac):
// a page that loaded with another one loads again.
void api<{ language?: string }>('/api/settings').then((s) => adoptLanguage(s.language), () => {})

createRoot(document.getElementById('root')!).render(<App />)
