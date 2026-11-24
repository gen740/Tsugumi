import { useCallback, useEffect } from 'react'
import { L } from '../i18n'
import type { CardLiveViews } from '../model/liveViews'
import { useObserve } from '../model/observable'
import { path, type CardKey, type ProjectSession } from '../model/session'
import { Empty, Spinner } from './controls'
import { DawnViewCanvas, type ReadDawnView } from './dawnViewCanvas'
import { HtmlViewFrame, type ReadHtmlView, type SendHtmlView } from './htmlViewFrame'

export function CardViewsWindow({ session, cardKey }: { session: ProjectSession; cardKey: CardKey }) {
  useObserve(session)
  useEffect(() => session.observeDisplay(cardKey), [session, cardKey])
  const client = session.rpc
  const read: ReadHtmlView = useCallback(async (view, signal) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.readCardView(path(cardKey), view.index, view.declaration.name, signal)
  }, [client, cardKey])
  const readBytes: ReadDawnView = useCallback(async (view, signal) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.readCardViewBytes(path(cardKey), view.index, view.declaration.name, signal)
  }, [client, cardKey])
  const send: SendHtmlView = useCallback(async (view, bytes) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.sendView(path(cardKey), view.declaration.name, bytes)
  }, [client, cardKey])
  return <div className="col fill pad16 gap8">
    <span className="title2">{session.displayName(cardKey)}</span>
    <CardViewsPanel card={session.liveViews.get(cardKey)} read={read} readBytes={readBytes} send={send} />
  </div>
}

export function CardViewsPanel({ card, read, readBytes, send }: { card?: CardLiveViews; read: ReadHtmlView; readBytes: ReadDawnView; send: SendHtmlView }) {
  if (card?.failure !== undefined)
    return <div className="card-views">
      <p className="caption" role="alert">{L('ビューの宣言を評価できませんでした: {0}', card.failure)}</p>
    </div>
  // Until the core has evaluated the declarations, there is nothing to say about them yet.
  if (!card?.declared)
    return <div className="center-fill"><Spinner /></div>
  if (!card.views.length && !card.unknown.size)
    return <Empty title={L('表示するビューがありません')} icon="chart.xyaxis.line" />
  return <div className="card-views">
    {card.views.map(view => {
      const { name, type, blob } = view.declaration
      return <section className={type === 'html' || type === 'dawn' ? `card-view card-view-${type}` : 'card-view'} key={JSON.stringify([view.index, name, type, blob])}>
        <h3>{name}</h3>
        {type === 'html' ? <HtmlViewFrame view={view} card={card} read={read} send={send} />
          : type === 'dawn' ? <DawnViewCanvas view={view} read={readBytes} send={send} />
          : <p className="caption" role="alert">{L('未対応のビュー形式: {0}', type)}</p>}
      </section>
    })}
    {[...card.ambiguous].map(name => <p className="caption" role="alert" key={`ambiguous:${name}`}>{L('ビュー名「{0}」が重複しているため、この名前に送られたデータはどのビューにも届きません', name)}</p>)}
    {[...card.unknown].map(name => <p className="caption" role="alert" key={name}>{L('宣言されていないビュー: {0}', name)}</p>)}
  </div>
}
