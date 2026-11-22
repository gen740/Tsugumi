import { useCallback, useEffect } from 'react'
import { L } from '../i18n'
import type { CardLiveViews } from '../model/liveViews'
import { useObserve } from '../model/observable'
import { path, type CardKey, type ProjectSession } from '../model/session'
import { Empty } from './controls'
import { HtmlViewFrame, type ReadHtmlView } from './htmlViewFrame'

export function CardViewsWindow({ session, cardKey }: { session: ProjectSession; cardKey: CardKey }) {
  useObserve(session)
  useEffect(() => session.observeDisplay(cardKey), [session, cardKey])
  const client = session.rpc
  const read: ReadHtmlView = useCallback(async (name, signal) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.readCardView(path(cardKey), name, signal)
  }, [client, cardKey])
  return <div className="col fill pad16 gap8">
    <span className="title2">{session.displayName(cardKey)}</span>
    <CardViewsPanel card={session.liveViews.get(cardKey)} read={read} />
  </div>
}

export function CardViewsPanel({ card, read }: { card?: CardLiveViews; read: ReadHtmlView }) {
  if (!card || (!card.views.size && !card.unknown.size))
    return <Empty title={L('表示するビューがありません')} icon="chart.xyaxis.line" />
  return <div className="card-views">
    {[...card.views.values()].map(view => {
      const { name, type, blob } = view.declaration
      return <section className={type === 'html' ? 'card-view card-view-html' : 'card-view'} key={JSON.stringify([name, type, blob])}>
        <h3>{name}</h3>
        {type === 'html' ? <HtmlViewFrame view={view} card={card} read={read} />
          : type === 'dawn' ? <p className="caption secondary">{L('Dawn ビューはまだ利用できません。')}</p>
          : <p className="caption" role="alert">{L('未対応のビュー形式: {0}', type)}</p>}
      </section>
    })}
    {[...card.unknown].map(name => <p className="caption" role="alert" key={name}>{L('宣言されていないビュー: {0}', name)}</p>)}
  </div>
}
