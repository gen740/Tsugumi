import { useEffect, useState } from 'react'
import * as H from '../../shared/gen/tsugumi/host'
import { L } from '../i18n'
import { errorText } from '../model/session'
import { hostCall } from '../wire/host'
import { s } from '../wire/rpc'
import { Button, Disclosure, Form, Row, Section, Select, TextField } from './controls'

type Target = { kind: H.CachePushKind; endpoint: string; cacheName: string }

export function BinaryCacheSettings() {
  return <Form>
    <CachePushSettings />
    <CachePullSettings />
  </Form>
}

function CachePushSettings() {
  const [stored, setStored] = useState<H.CachePushTargetT>()
  const [edit, setEdit] = useState<Target>()
  const [user, setUser] = useState('')
  const [secret, setSecret] = useState('')
  const [saving, setSaving] = useState(false)
  const [problem, setProblem] = useState<string>()
  const { Own, Attic, Cachix, NixCopy } = H.CachePushKind

  function adopt(target: H.CachePushTargetT) {
    setStored(target)
    setEdit({ kind: target.kind, endpoint: s(target.endpoint), cacheName: s(target.cacheName) })
  }
  useEffect(() => {
    void hostCall<H.CachePushTargetT>(H.RequestPayload.CachePushTargetGetRequest,
      new H.CachePushTargetGetRequestT(), H.ReplyPayload.CachePushTarget)
      .then(adopt, (error) => setProblem(errorText(error)))
  }, [])

  const same = !!edit && edit.kind === stored?.kind && edit.endpoint === s(stored.endpoint) &&
    edit.cacheName === s(stored.cacheName)
  const needsEndpoint = edit?.kind === Attic || edit?.kind === NixCopy
  const needsName = edit?.kind === Attic || edit?.kind === Cachix
  const complete = !!edit && (!needsEndpoint || !!edit.endpoint) && (!needsName || !!edit.cacheName)
  const s3 = edit?.endpoint.startsWith('s3://')
  const usesPair = edit?.kind === NixCopy && (s3 || edit.endpoint.startsWith('https://'))

  async function save(target: Target, withCredentials = false, clear = false) {
    setSaving(true)
    try {
      const request = new H.CachePushTargetPutRequestT(target.kind, target.endpoint, target.cacheName,
        withCredentials && usesPair ? user : null, withCredentials ? secret : null, clear)
      adopt(await hostCall<H.CachePushTargetT>(H.RequestPayload.CachePushTargetPutRequest,
        request, H.ReplyPayload.CachePushTarget))
      if (withCredentials) { setUser(''); setSecret('') }
      setProblem(undefined)
    } catch (error) {
      setProblem(errorText(error))
    } finally {
      setSaving(false)
    }
  }

  useEffect(() => {
    if (!edit || same || !complete || saving) return
    const timer = setTimeout(() => void save(edit), 600)
    return () => clearTimeout(timer)
  }, [edit, same, complete]) // Saving does not reschedule an unsuccessful edit.

  return <Section title={L('キャッシュへの送信先')}>
      {edit && <fieldset disabled={saving} style={{ border: 0, margin: 0, padding: 0 }}>
        <Row label={L('種類')}>
          <Select value={edit.kind} options={[
            { value: Own, label: L('Tsugumi のキャッシュ') },
            { value: Attic, label: 'Attic' },
            { value: Cachix, label: 'Cachix' },
            { value: NixCopy, label: 'nix copy' },
          ]} onChange={kind => {
            if (kind === stored?.kind) adopt(stored)
            else setEdit({ kind, endpoint: '', cacheName: '' })
            setUser('')
            setSecret('')
            setProblem(undefined)
          }} />
        </Row>
        {needsEndpoint && <Row label={L('キャッシュ URL')}>
          <TextField value={edit.endpoint} onChange={endpoint => setEdit({ ...edit, endpoint })} placeholder="https://cache.example.org" />
        </Row>}
        {needsName && <Row label={L('キャッシュ名')}>
          <TextField value={edit.cacheName} onChange={cacheName => setEdit({ ...edit, cacheName })} />
        </Row>}
        {(usesPair || edit.kind === Attic || edit.kind === Cachix) && <Disclosure label={L('認証情報')}>
          {usesPair && <Row label={s3 ? L('アクセスキー ID') : L('ユーザー名')}>
            <input className="field" type="password" autoComplete="off" value={user} onChange={e => setUser(e.target.value)} />
          </Row>}
          <Row label={L('パスワード・token')}>
            <input className="field" type="password" autoComplete="new-password" value={secret} onChange={e => setSecret(e.target.value)} />
          </Row>
          <Row>
            <Button disabled={!same || !secret || (!!usesPair && !user)} onClick={() => void save(edit, true)}>{L('認証情報を設定')}</Button>
            {same && stored?.hasCredentials && <Button onClick={() => void save(edit, false, true)}>{L('認証情報を削除')}</Button>}
          </Row>
        </Disclosure>}
      </fieldset>}
      {problem && <Row>{problem}</Row>}
  </Section>
}

type PullCache = { id: string; name: string; url: string; publicKey: string; username: string; password: string }

const pullFields = ['name', 'url', 'publicKey'] as const
const blankPull = (cache: PullCache) => [...pullFields, 'username', 'password'].every(key => !cache[key as keyof PullCache])

function CachePullSettings() {
  const [caches, setCaches] = useState<PullCache[]>()
  const [saved, setSaved] = useState<string>()
  const [problem, setProblem] = useState<string>()
  const [saving, setSaving] = useState(false)
  useEffect(() => {
    void hostCall<H.BinaryCachesT>(H.RequestPayload.BinaryCachesGetRequest,
      new H.BinaryCachesGetRequestT(), H.ReplyPayload.BinaryCaches)
      .then(result => {
        const entries = JSON.parse(s(result.cachesJson)) as PullCache[]
        setCaches(entries)
        setSaved(JSON.stringify(entries))
      }).catch(error => setProblem(errorText(error)))
  }, [])
  const update = (id: string, change: Partial<PullCache>) => {
    setCaches(old => old?.map(cache => cache.id === id ? { ...cache, ...change } : cache))
  }
  const encoded = caches && JSON.stringify(caches.filter(cache => !blankPull(cache)))
  useEffect(() => {
    if (!encoded || saved === undefined || encoded === saved || saving) return
    const timer = setTimeout(async () => {
      setSaving(true)
      try {
        await hostCall<H.BinaryCachesT>(H.RequestPayload.BinaryCachesPutRequest,
          new H.BinaryCachesPutRequestT(encoded), H.ReplyPayload.BinaryCaches)
        setSaved(encoded)
        setProblem(undefined)
      } catch (error) {
        setProblem(errorText(error))
      } finally {
        setSaving(false)
      }
    }, 600)
    return () => clearTimeout(timer)
  }, [encoded, saved])
  return <Section title={L('キャッシュ接続')}>
    {caches?.map(cache => <fieldset key={cache.id} disabled={saving}>
      <Row label={L('名前')}><TextField value={cache.name} onChange={name => update(cache.id, { name })} /></Row>
      <Row label={L('キャッシュ URL')}><TextField value={cache.url} onChange={url => update(cache.id, { url })} /></Row>
      <Row label={L('署名の公開鍵')}><TextField value={cache.publicKey} onChange={publicKey => update(cache.id, { publicKey })} mono /></Row>
      <Disclosure label={L('認証（非公開キャッシュ）')}>
        <Row label={L('ユーザー名')}><TextField value={cache.username} onChange={username => update(cache.id, { username })} /></Row>
        <Row label={L('パスワード・token')}>
          <input className="field" type="password" autoComplete="new-password" value={cache.password} onChange={e => update(cache.id, { password: e.target.value })} />
        </Row>
      </Disclosure>
      <Row><Button onClick={() => setCaches(caches.filter(next => next.id !== cache.id))}>{L('キャッシュ接続を削除')}</Button></Row>
    </fieldset>)}
    {caches && <Row><Button disabled={saving} onClick={() => {
      const random = [...crypto.getRandomValues(new Uint8Array(16))].map(value => value.toString(16).padStart(2, '0')).join('')
      const id = random.replace(/^(.{8})(.{4})(.{4})(.{4})(.{12})$/, '$1-$2-$3-$4-$5')
      setCaches([...caches, { id, name: '', url: '', publicKey: '', username: '', password: '' }])
    }}>{L('キャッシュ接続を追加')}</Button></Row>}
    {problem && <Row>{problem}</Row>}
  </Section>
}
