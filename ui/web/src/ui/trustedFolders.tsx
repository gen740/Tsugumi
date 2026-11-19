import { useEffect, useState } from 'react'
import { api, onServerEvent } from '../api'
import { errorText } from '../model/session'
import { L } from '../i18n'
import { Button, Row, Section } from './controls'
import { FolderPicker } from './sidebar'

export function TrustedFoldersSettings() {
  const [settings, setSettings] = useState<{ trustedFolders: string[]; revision: number }>()
  const [picking, setPicking] = useState(false)
  const [error, setError] = useState<string>()
  const [saving, setSaving] = useState(false)
  const load = async () => setSettings(await api('/api/settings'))
  useEffect(() => {
    const refresh = () => void load().catch(e => setError(errorText(e)))
    refresh()
    return onServerEvent(e => { if (e.type === 'settingsChanged') refresh() })
  }, [])
  const save = async (trustedFolders: string[]) => {
    if (!settings || saving) return
    setSaving(true)
    try {
      await api('/api/settings', { revision: settings.revision, trustedFolders })
      setError(undefined)
    } catch (e) { setError(errorText(e)) }
    finally {
      try {
        await load()
      } catch (e) { setError(errorText(e)) }
      setSaving(false)
    }
  }
  return <Section title={L('信頼するフォルダ')} info={L('このフォルダ内のプロジェクトを、下の階層も含めて信頼します。シンボリックリンクの参照先が外にあるプロジェクトは含みません。')}>
    {settings?.trustedFolders.map(folder => <Row key={folder} label={folder}>
      <Button disabled={saving} onClick={() => void save(settings.trustedFolders.filter(f => f !== folder))}>{L('削除')}</Button>
    </Row>)}
    <Row><Button disabled={!settings || saving} onClick={() => setPicking(true)}>{L('フォルダを追加…')}</Button></Row>
    {error && <Row><span className="error">{error}</span></Row>}
    {picking && <FolderPicker message={L('信頼するフォルダ')} onPick={folder => { setPicking(false); void save([...(settings?.trustedFolders ?? []), folder]) }} onClose={() => setPicking(false)} />}
  </Section>
}
