const locale = document.documentElement.lang
const L = (ja, value) => (locale === 'ja' ? ja : english[ja] ?? ja)
  .replace('{0}', () => String(value))
const $ = id => document.getElementById(id)
for (const node of document.querySelectorAll('[data-l]')) node.textContent = L(node.dataset.l)
$('cache-key').placeholder = L("公開鍵")

const states = {
  missing: L("記録なし"), uploading: L("転送中"), queued: L("実行待機中"),
  running: L("実行中"), cancelling: L("キャンセル中"), succeeded: L("完了"),
  failed: L("失敗"), cancelled: L("キャンセル済み"), interrupted: L("中断"),
  released: L("解放済み"), unreadable: L("読み取り不能"),
}
// Recorded as queued, running or cancelling: the worker removes none of these.
const active = new Set(['queued', 'running', 'cancelling'])
function jobReason(job, admin) {
  switch (job.state) {
    case 'missing': return L("ジャーナルへの初回書き込み前に転送が停止しました。")
    case 'uploading': return L("入力は配置済みですが、ジョブは送信されていません。")
    case 'queued': return admin ? L("実行待ち（削除できません）") : L("実行待ち")
    case 'running':
    case 'cancelling': return admin ? L("実行中（削除できません）") : L("実行中")
    case 'succeeded': return L("ホストがまだ結果を受け取っていません")
    case 'failed':
    case 'cancelled':
    case 'interrupted': return L("入力、ログ、途中結果が残っています。")
    case 'released': return L("解放済みです。リスナーが残りの削除を完了します。")
    default: return job.reason
  }
}
function stateIcon(state) {
  if (state === 'running' || state === 'cancelling') return 'run'
  if (state === 'succeeded') return 'done'
  if (state === 'failed' || state === 'unreadable') return 'failed'
  return 'idle'
}

const selected = new Set()
let current, busy = false, refreshing = false, dirty = false, lockTimer
const formatBytes = new Intl.NumberFormat(locale, { maximumFractionDigits: 1 })
function size(bytes) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let i = 0
  while (bytes >= 1000 && i < units.length - 1) { bytes /= 1000; i++ }
  return formatBytes.format(bytes) + ' ' + units[i]
}
function duration(seconds) {
  const days = Math.floor(seconds / 86400), hours = Math.floor(seconds / 3600) % 24
  const minutes = Math.floor(seconds / 60) % 60
  if (days) return L("{0} 日", days) + ' ' + L("{0} 時間", hours)
  if (hours) return L("{0} 時間", hours) + ' ' + L("{0} 分", minutes)
  return L("{0} 分", minutes)
}
function ago(iso) {
  const seconds = Math.max(0, (Date.now() - Date.parse(iso)) / 1000)
  if (seconds < 60) return L("たった今")
  if (seconds < 3600) return L("{0} 分前", Math.floor(seconds / 60))
  if (seconds < 86400) return L("{0} 時間前", Math.floor(seconds / 3600))
  return L("{0} 日前", Math.floor(seconds / 86400))
}
function element(tag, text, className) {
  const node = document.createElement(tag)
  if (text !== undefined) node.textContent = text
  if (className) node.className = className
  return node
}
function button(text, className, action) {
  const node = element('button', text, 'btn' + (className ? ' ' + className : ''))
  node.type = 'button'
  node.addEventListener('click', action)
  return node
}
function report(text, error = false) {
  $('feedback').textContent = text
  $('feedback').toggleAttribute('data-error', error)
}
const admin = () => current?.role === 'admin'

async function call(path, body) {
  const response = await fetch(path, body === undefined ? { cache: 'no-store' } : {
    method: 'POST', cache: 'no-store',
    headers: { 'Content-Type': 'application/json', 'X-Tsugumi-CSRF': current?.csrf ?? '' },
    body: JSON.stringify(body),
  })
  const data = await response.json().catch(() => ({}))
  return { status: response.status, data }
}
async function change(path, body, done) {
  if (busy) return
  busy = true
  controls()
  report(L("処理中…"))
  try {
    const { status, data } = await call(path, body)
    if (path === '/api/password' && status !== 200) {
      const message = passwordError(data)
      if (message) {
        report(message, true)
        return
      }
    }
    if (status === 401) {
      report(L("ログインの有効期限が切れました。もう一度ログインしてください。"), true)
      await refresh()
      return
    }
    if (status !== 200) throw new Error(data.error ?? String(status))
    report('')
    await done?.(data)
  } catch (e) {
    report(L("変更を完了できませんでした: {0}", e.message), true)
  } finally {
    busy = false
    await refresh()
  }
}

function passwordError({ error, locked_seconds }) {
  if (locked_seconds > 0)
    return L("あと {0} 分でパスワードを確認できます", Math.ceil(locked_seconds / 60))
  switch (error) {
    case 'wrong_password': return L("現在のパスワードが違います。")
    case 'password_length': return L("8〜1024 バイトのパスワードを入力してください。")
    case 'password_mismatch': return L("新しいパスワードが一致しません。")
    case 'not_configured': return L("管理者パスワードが設定されていません")
  }
}

function controls() {
  const allowed = admin() && !busy
  $('remove-jobs').disabled = !allowed || !selected.size
  $('remove-jobs').textContent = L("選択した {0} 件を削除…", selected.size)
  $('save-settings').disabled = !allowed || !dirty
  $('add-key').disabled = !allowed || !$('host-key').value.trim()
  $('add-cache').disabled = !allowed || !$('cache-url').value.trim() || !$('cache-key').value.trim()
  $('password-edit-submit').disabled = !allowed
  for (const input of $('jobs').querySelectorAll('input'))
    input.disabled = !allowed || input.dataset.active === '1' || (!input.checked && selected.size >= 256)
}

function renderHeader(data) {
  const { worker } = data
  document.title = L("{0} のワーカー管理", worker.name)
  $('name').textContent = worker.name
  $('meta').textContent = ['tsugumi-workerd ' + worker.version, L("契約 {0}", worker.contract),
    L("リビジョン {0}", worker.revision || L("不明")), worker.system,
    L("起動から {0}", duration(worker.uptime_seconds))].join(' · ')
  document.body.classList.toggle('admin', admin())
  $('badge').textContent = admin() ? L("管理者") : L("閲覧のみ（guest）")
  $('badge').classList.toggle('admin', admin())
  $('login-open').hidden = admin()
  $('restart-open').hidden = !admin()
  $('logout').hidden = !admin()
  $('password-edit').hidden = !admin()
  if (!admin()) $('password-edit-form').reset()
  $('notice').hidden = !worker.restart_pending
  $('notice').textContent = L("実行中のジョブが終わると再起動します。新しいジョブはそれまで始まりません。")
  $('login-title').textContent = L("{0} の管理者ログイン", worker.name)
}
function renderJobs(data) {
  const names = new Set(data.jobs.map(job => job.project_scope + '/' + job.job_id))
  for (const name of selected) if (!names.has(name) || !admin()) selected.delete(name)
  const rows = data.jobs.map(job => {
    const name = job.project_scope + '/' + job.job_id
    const tr = element('tr')
    const cell = (value, className) => {
      const td = element('td', undefined, className)
      td.append(value)
      tr.append(td)
      return td
    }
    const input = element('input')
    input.type = 'checkbox'
    input.checked = selected.has(name)
    input.dataset.active = active.has(job.state) ? '1' : ''
    input.setAttribute('aria-label', L("ジョブ {0} を選択", job.job_id.slice(0, 12)))
    input.addEventListener('change', () => {
      if (input.checked) selected.add(name)
      else selected.delete(name)
      controls()
    })
    cell(input, 'admin-only')
    const state = element('span', undefined, 'state')
    state.append(element('span', undefined, 'dot ' + stateIcon(job.state)), states[job.state] ?? job.state)
    cell(state)
    const scope = element('code', job.project_scope.slice(0, 12) + '…')
    scope.title = job.project_scope
    cell(scope)
    const id = element('code', job.job_id.slice(0, 12) + '…')
    id.title = job.job_id
    cell(id)
    cell(size(job.bytes), 'number')
    const time = element('time', ago(job.last_written))
    time.dateTime = job.last_written
    time.title = job.last_written.replace('T', ' ').replace('Z', ' UTC')
    cell(time, 'number')
    cell(jobReason(job, admin()), 'why')
    return tr
  })
  $('jobs').replaceChildren(...rows)
  $('empty-jobs').hidden = !!rows.length
  $('jobs-note').textContent = L("{0} 件 · 自動では削除されません", rows.length)
  $('jobs-note').hidden = admin()
  $('remove-jobs').hidden = !admin()
}
function renderDisk({ disk }) {
  const bar = bytes => {
    const value = element('div', size(bytes), 'number')
    const track = element('div', undefined, 'bar')
    const fill = element('i')
    fill.style.width = (disk.total_bytes ? Math.min(100, 100 * bytes / disk.total_bytes) : 0).toFixed(1) + '%'
    track.append(fill)
    value.append(track)
    return value
  }
  $('disk').replaceChildren(
    element('div', L("Nix ストア（このワーカー専用）")), bar(disk.store_bytes),
    element('div', L("ジョブの作業領域")), bar(disk.jobs_bytes),
    element('div', L("空き")), element('div', size(disk.free_bytes), 'number'))
}
function listen(settings, worker) {
  return worker.listen + ' · ' + L("ダッシュボード") + ' :' + worker.dashboard_port
}
function renderSettingsView({ settings, worker }) {
  const nix = value => value ? String(value) : L("マシンの設定")
  const caches = element('div')
  if (!settings.pull_caches.length) caches.textContent = L("なし")
  settings.pull_caches.forEach((cache, i) => {
    if (i) caches.append(element('br'))
    caches.append(element('code', cache.url))
  })
  $('settings-values').replaceChildren(
    element('div', L("同時に動かすジョブ")), element('div', String(settings.max_jobs)),
    element('div', L("Nix のビルド並列度")),
    element('div', 'max-jobs ' + nix(settings.nix_max_jobs) + ' · cores ' + nix(settings.nix_cores)),
    element('div', L("pull キャッシュ")), caches,
    element('div', L("待ち受け")), element('div', listen(settings, worker)))
  $('settings-view').hidden = admin()
  $('settings-edit').hidden = !admin()
}
function cacheRow(cache) {
  const line = element('li')
  line.dataset.url = cache.url
  line.dataset.key = cache.public_key
  const label = element('span')
  label.append(element('code', cache.url), element('br'), element('small', cache.public_key))
  line.append(label, button(L("削除"), 'danger', () => {
    line.remove()
    edited()
  }))
  return line
}
function renderSettingsForm({ settings, worker }) {
  $('restart-needed').hidden = !worker.restart_required
  if (dirty) return
  $('name-input').value = settings.name
  $('max-jobs').value = settings.max_jobs
  $('nix-max-jobs').value = settings.nix_max_jobs
  $('nix-cores').value = settings.nix_cores
  $('listen-address').value = settings.listen_address
  $('port').value = settings.port
  $('dashboard-port').value = settings.dashboard_port
  $('pull-caches').replaceChildren(...settings.pull_caches.map(cacheRow))
}
function renderHosts({ hosts }) {
  $('hosts-note').textContent = L("authorized_keys · {0} 件", hosts.length)
  $('hosts').replaceChildren(...hosts.map(host => {
    const line = element('li')
    const label = element('span')
    const key = element('code', host.key.length > 44 ? host.key.slice(0, 44) + '…' : host.key)
    key.title = host.key
    label.append(key)
    if (host.comment) label.append(' ', host.comment)
    line.append(label)
    if (admin()) {
      line.append(button(L("削除"), 'danger', () => ask(L("ホスト鍵を削除しますか"),
        L("このホストはワーカーに接続できなくなります: {0}", host.key), [
          [L("削除する"), 'danger', () => change('/api/keys/remove', { key: host.key })],
        ])))
    } else {
      line.append(element('small', host.last_seen ? L("最終接続 {0}", ago(host.last_seen)) : L("起動後は未接続")))
    }
    return line
  }))
}
function render(data) {
  current = data
  renderHeader(data)
  renderJobs(data)
  renderDisk(data)
  renderSettingsView(data)
  if (admin()) renderSettingsForm(data)
  else dirty = false
  renderHosts(data)
  controls()
}
async function refresh() {
  if (refreshing) return
  refreshing = true
  try {
    const { status, data } = await call('/api/state')
    if (status !== 200) throw new Error(data.error ?? String(status))
    render(data)
    if ($('feedback').dataset.refreshError === '1') report('')
    $('feedback').dataset.refreshError = ''
  } catch (e) {
    report(L("ワーカーの情報を取得できません: {0}", e.message), true)
    $('feedback').dataset.refreshError = '1'
  } finally {
    refreshing = false
  }
}

function ask(title, text, choices) {
  $('confirm-title').textContent = title
  $('confirm-text').textContent = text
  const close = () => { $('confirm').hidden = true }
  $('confirm-choices').replaceChildren(...choices.map(([label, kind, action]) =>
    button(label, kind, () => { close(); void action() })),
  button(L("やめる"), '', close))
  $('confirm').hidden = false
  $('confirm-choices').lastElementChild.focus()
}
function edited() {
  dirty = true
  controls()
}
function number(id) {
  const value = Number($(id).value)
  if ($(id).value.trim() === '' || !Number.isInteger(value) || value < 0) {
    $(id).focus()
    throw new Error(L("0 以上の整数を入力してください。"))
  }
  return value
}

$('login-open').addEventListener('click', () => {
  $('login').hidden = false
  $('password').value = ''
  showLock(current?.login.locked_seconds ?? 0)
  if (current && !current.login.configured) {
    $('login-error').textContent = L("管理者パスワードが設定されていません")
    $('password').disabled = $('login-submit').disabled = true
  }
  if (!$('password').disabled) $('password').focus()
})
$('login-cancel').addEventListener('click', () => { $('login').hidden = true })
function showLock(seconds) {
  clearTimeout(lockTimer)
  $('password').disabled = $('login-submit').disabled = seconds > 0
  $('login-error').textContent = seconds > 0 ? L("あと {0} 分でログインできるようになります", Math.ceil(seconds / 60)) : ''
  if (seconds > 0) lockTimer = setTimeout(() => showLock(Math.max(0, seconds - 30)), 30000)
}
$('login-form').addEventListener('submit', async event => {
  event.preventDefault()
  if ($('login-submit').disabled) return
  $('login-submit').disabled = true
  try {
    const { status, data } = await call('/api/login', { password: $('password').value })
    $('password').value = ''
    if (status === 200) {
      $('login').hidden = true
      report('')
      await refresh()
      return
    }
    if (status === 403) {
      showLock(0)
      $('login-error').textContent = L("管理者パスワードが設定されていません")
      return
    }
    if (data.locked_seconds) showLock(data.locked_seconds)
    else {
      showLock(0)
      $('login-error').textContent = L("パスワードが違います")
      $('password').focus()
    }
  } catch (e) {
    $('login-submit').disabled = false
    $('login-error').textContent = e.message
  }
})
$('logout').addEventListener('click', () => void change('/api/logout', {}))
$('password-edit-form').addEventListener('submit', event => {
  event.preventDefault()
  if ($('password-edit-submit').disabled) return
  const body = {
    current_password: $('current-password').value,
    new_password: $('new-password').value,
    confirm_password: $('confirm-password').value,
  }
  event.currentTarget.reset()
  void change('/api/password', body, () => {
    report(L("パスワードを変更し、他のセッションをログアウトしました。"))
  })
})
$('restart-open').addEventListener('click', () => {
  const running = current.worker.running
  ask(L("{0} を再起動しますか", current.worker.name),
    running ? L("実行中のジョブが {0} 件あります。", running) : L("実行中のジョブはありません。"), [
      [L("実行中のジョブが終わってから再起動"), 'primary', () => restart('after_jobs')],
      [L("今すぐ再起動（実行中のジョブは止まります）"), 'danger', () => restart('now')],
    ])
})
function restart(mode) {
  return change('/api/restart', { mode }, () => {
    report(mode === 'now' ? L("再起動しています…") : L("実行中のジョブが終わると再起動します。新しいジョブはそれまで始まりません。"))
  })
}
$('remove-jobs').addEventListener('click', () => ask(L("ジョブのデータを削除しますか"),
  L("選択した {0} 件のジョブのデータをワーカーから削除します。元に戻せません。", selected.size), [
    [L("削除する"), 'danger', () => change('/api/jobs/remove', { jobs: [...selected] }, data => {
      const failed = data.results.filter(row => row.status !== 'removed' && row.status !== 'released')
      for (const row of data.results) if (!failed.includes(row)) selected.delete(row.job)
      report(failed.length
        ? failed.map(row => row.job.slice(65, 77) + ' · ' + row.detail).join(' ')
        : L("{0} 件のジョブを削除しました。", data.results.length), !!failed.length)
    })],
  ]))
$('save-settings').addEventListener('click', () => {
  let settings
  try {
    settings = {
      name: $('name-input').value.trim(),
      listen_address: $('listen-address').value.trim(),
      port: number('port'), dashboard_port: number('dashboard-port'),
      max_jobs: number('max-jobs'), nix_max_jobs: number('nix-max-jobs'), nix_cores: number('nix-cores'),
      pull_caches: [...$('pull-caches').children].map(line => ({ url: line.dataset.url, public_key: line.dataset.key })),
    }
  } catch (e) {
    report(e.message, true)
    return
  }
  void change('/api/settings', { settings }, data => {
    dirty = false
    render(data)
    report(L("設定を保存しました。"))
  })
})
for (const id of ['name-input', 'max-jobs', 'nix-max-jobs', 'nix-cores', 'listen-address', 'port', 'dashboard-port'])
  $(id).addEventListener('input', edited)
$('add-cache').addEventListener('click', () => {
  $('pull-caches').append(cacheRow({ url: $('cache-url').value.trim(), public_key: $('cache-key').value.trim() }))
  $('cache-url').value = $('cache-key').value = ''
  edited()
})
for (const id of ['cache-url', 'cache-key', 'host-key']) $(id).addEventListener('input', controls)
$('add-key').addEventListener('click', () => {
  const key = $('host-key').value.trim()
  void change('/api/keys/add', { key }, () => {
    $('host-key').value = ''
    report(L("ホスト鍵を追加しました。"))
  })
})
addEventListener('keydown', event => {
  if (event.key === 'Escape') { $('confirm').hidden = true; $('login').hidden = true }
})
void refresh()
setInterval(() => { if (!busy) void refresh() }, 5000)
