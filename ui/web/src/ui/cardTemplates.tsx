import { useEffect, useState } from 'react'
import { L } from '../i18n'
import { errorText, TEMPLATES, type DefinitionTemplate, type ProjectSession } from '../model/session'
import * as H from '../../shared/gen/tsugumi/host'
import { s } from '../wire/rpc'
import { Button, Select, Sheet, Spinner, TextField, type MenuItem } from './controls'
import { FolderPicker } from './sidebar'

export type TemplatePlacement = { scope: string[]; at: { x: number; y: number } }

export function newCardItems(add: (kind: DefinitionTemplate) => void, pickTemplate: () => void): MenuItem[] {
  return [
    ...TEMPLATES.map(({ id, label }) => ({ label, action: () => add(id) })),
    'divider',
    { label: L('テンプレートから…'), action: pickTemplate },
  ]
}

const stores = [
  { value: H.TemplateStore.Project, label: L('プロジェクト') },
  { value: H.TemplateStore.Host, label: L('ホスト') },
]

export function TemplatePicker({ session, placement, onClose }: {
  session: ProjectSession; placement: TemplatePlacement; onClose: () => void
}) {
  const [entries, setEntries] = useState<H.TemplateEntryT[]>()
  const [mode, setMode] = useState<'list' | 'folder' | 'git'>('list')
  const [browse, setBrowse] = useState(false)
  const [store, setStore] = useState<H.TemplateStore>()
  const [name, setName] = useState('')
  const [folder, setFolder] = useState('')
  const [url, setUrl] = useState('')
  const [ref, setRef] = useState('')
  const [subdirectory, setSubdirectory] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string>()

  useEffect(() => {
    let active = true
    void session.listTemplates().then(items => { if (active) setEntries(items) },
      failure => { if (active) setError(errorText(failure)) })
    return () => { active = false }
  }, [session])

  const save = async () => {
    if (store === undefined || busy) return
    setBusy(true)
    setError(undefined)
    const addition = mode === 'git'
      ? new H.TemplateAddT(name.trim(), H.TemplateSource.TemplateGit,
        new H.TemplateGitT(url.trim(), ref.trim(), subdirectory.trim()))
      : new H.TemplateAddT(name.trim(), H.TemplateSource.TemplateFolder, new H.TemplateFolderT(folder))
    try {
      await session.addTemplate(store, addition)
      setMode('list')
      setEntries(await session.listTemplates())
    } catch (failure) {
      setError(errorText(failure))
    } finally {
      setBusy(false)
    }
  }

  const copy = async (entry: H.TemplateEntryT) => {
    setBusy(true)
    setError(undefined)
    try {
      await session.copyTemplate(entry, placement.scope, placement.at)
      onClose()
    } catch (failure) {
      setError(errorText(failure))
      setBusy(false)
    }
  }

  const chooseSource = (next: 'folder' | 'git') => {
    setStore(undefined)
    setName('')
    setError(undefined)
    setMode(next)
  }
  const close = () => { if (!busy) onClose() }
  if (browse) return <FolderPicker initial={folder || undefined} message={L('テンプレートのフォルダを選んでください')}
    onPick={path => {
      setFolder(path)
      if (!name) setName(path.split('/').filter(Boolean).at(-1) ?? '')
      setBrowse(false)
    }} onClose={() => setBrowse(false)} />

  return <Sheet width={480} onClose={close} backdropCloses={!busy}>
    <div className="sheet-body">
      <div className="headline">{mode === 'list' ? L('テンプレートから…')
        : mode === 'folder' ? L('テンプレートを追加…') : L('Git からインポート…')}</div>
      {mode === 'list' ? <>
        {!entries && !error && <Spinner />}
        {stores.map(item => {
          const items = entries?.filter(entry => entry.store === item.value)
          return <section key={item.value}>
            <h3>{item.label}</h3>
            {items?.length ? <div className="col gap4">{items.map(entry =>
              <Button key={s(entry.name)} disabled={busy || !session.isEditable}
                onClick={() => void copy(entry)}>{s(entry.name)}</Button>)}</div>
              : items && <div className="secondary">{L('テンプレートがありません')}</div>}
          </section>
        })}
        <div className="row gap8">
          <Button disabled={busy} onClick={() => chooseSource('folder')}>{L('テンプレートを追加…')}</Button>
          <Button disabled={busy} onClick={() => chooseSource('git')}>{L('Git からインポート…')}</Button>
          <span className="spacer" />
          <Button disabled={busy} onClick={close}>{L('閉じる')}</Button>
        </div>
      </> : <form className="col gap8" onSubmit={event => { event.preventDefault(); void save() }}>
        <label className="col gap4">{L('保存先')}
          <Select<string> value={store === undefined ? '' : String(store)} onChange={value => setStore(value === '' ? undefined : Number(value) as H.TemplateStore)}
            options={[{ value: '', label: L('選択') }, ...stores.map(item => ({ ...item, value: String(item.value) }))]} />
        </label>
        <label className="col gap4">{L('名前')}<TextField value={name} onChange={setName} /></label>
        {mode === 'git' ? <>
          <label className="col gap4">{L('公開 Git リポジトリの HTTPS URL')}<TextField value={url} onChange={setUrl} mono /></label>
          <label className="col gap4">{L('Git のブランチ、タグ、コミット')}<TextField value={ref} onChange={setRef} mono /></label>
          <label className="col gap4">{L('テンプレートのサブディレクトリ')}<TextField value={subdirectory} onChange={setSubdirectory} mono /></label>
        </> : <div className="row gap8 center">
          <Button disabled={busy} onClick={() => setBrowse(true)}>{L('フォルダを選択…')}</Button>
          <span className="secondary selectable">{folder}</span>
        </div>}
        <div className="row gap8 end">
          <Button disabled={busy} onClick={() => { setError(undefined); setMode('list') }}>{L('戻る')}</Button>
          <Button kind="prominent" type="submit"
            disabled={busy || !session.isEditable || store === undefined || !name.trim() ||
              (mode === 'folder' ? !folder : !url.trim() || !ref.trim() || !subdirectory.trim())}>
            {mode === 'folder' ? L('追加') : L('インポート')}
          </Button>
        </div>
      </form>}
      {busy && <Spinner />}
      {error && <div role="alert" className="callout selectable" style={{ color: 'var(--red)' }}>{error}</div>}
    </div>
  </Sheet>
}
