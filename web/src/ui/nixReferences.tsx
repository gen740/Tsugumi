import { useRef } from 'react'
import { L } from '../i18n'
import { app } from '../model/app'
import { canvasFor } from '../model/canvas'
import { nixReferences } from '../model/nixReferences'
import { useObserve } from '../model/observable'
import { CardKey, path, ProjectSession } from '../model/session'
import { WorkflowTab } from '../model/workspace'
import { Button, Popover } from './controls'

export function NixReferenceControls({ session, tab }: { session: ProjectSession; tab: WorkflowTab }) {
  useObserve(session, tab)
  const anchor = useRef<HTMLDivElement>(null)
  const refs = nixReferences(session)
  const jump = (card: CardKey) => {
    tab.enter(path(card).slice(0, -1)); tab.setSelection([card])
    canvasFor(session, tab).reveal(card)
    tab.showNixList = false; tab.changed()
  }
  return <div className="capsule material" ref={anchor}>
    <Button kind="borderless" icon="snowflake" pressed={tab.showNixReferences} onClick={() => {
      tab.showNixReferences = !tab.showNixReferences; tab.hoveredNixCard = undefined; tab.changed()
    }}>{L('Nix 参照')}</Button>
    <Button kind="borderless" icon="list.bullet" help={L('Nix 参照一覧')} onClick={() => { tab.showNixList = !tab.showNixList; tab.changed() }}>{L('一覧')}</Button>
    <Popover open={tab.showNixList} onClose={() => { tab.showNixList = false; tab.changed() }} anchor={anchor} width={460}>
      <div className="pad10 col gap8" style={{ maxHeight: '65vh', overflowY: 'auto' }}>
        <h3>{L('Nix 参照一覧')}</h3>
        <p className="caption secondary">{L('参照元 → 参照先。保存済みの default.nix にある静的な Card 参照を表示します。')}</p>
        {!refs.length && <p>{L('Card 間の Nix 参照はありません')}</p>}
        {refs.map(ref => <div key={ref.id} ref={node => { if (node && tab.selectedNixReference === ref.id) node.scrollIntoView({ block: 'nearest' }) }} className="pad6 col gap6" style={{ border: `1px solid ${tab.selectedNixReference === ref.id ? 'var(--purple)' : 'var(--separator)'}`, borderRadius: 6 }}>
          <div className="row gap8">
            <Button onClick={() => jump(ref.source)}>{ref.source}</Button><span>→</span>
            <Button disabled={!session.resolution.has(ref.target)} onClick={() => jump(ref.target)}>{ref.target}</Button>
          </div>
          <code className="selectable wrap-pre">{ref.expression}</code>
          {!ref.valid && <span className="secondary">{L('参照を解決できません。Nix 定義と診断を確認してください。')}</span>}
          <Button kind="borderless" onClick={() => { tab.showNixList = false; tab.changed(); app.openWindow(session, { kind: 'card', card: ref.source, category: 'nix' }) }}>{L('Nix 定義を開く')}</Button>
        </div>)}
      </div>
    </Popover>
  </div>
}
