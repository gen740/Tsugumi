import { createRoot } from 'react-dom/client'
import './styles.css'
import './styles/canvas.css'
import './styles/demoPlayer.css'
import './styles/detail.css'
import './styles/shell.css'
import './styles/chat.css'
import './styles/guide.css'
import { App } from './ui/App'

createRoot(document.getElementById('root')!).render(<App />)
