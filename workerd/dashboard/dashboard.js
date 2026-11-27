'use strict'

const locale = document.documentElement.lang
const L = (ja, value) => (locale === 'ja' ? ja : english[ja] ?? ja)
  .replace('{0}', () => String(value))
const $ = id => document.getElementById(id)
for (const node of document.querySelectorAll('[data-l]')) node.textContent = L(node.dataset.l)
document.title = L("ワーカー管理")

const states = {
  missing: L("記録なし"), uploading: L("転送中"), queued: L("実行待機中"),
  running: L("実行中"), cancelling: L("キャンセル中"), succeeded: L("成功"),
  failed: L("失敗"), cancelled: L("キャンセル済み"), interrupted: L("中断"),
  released: L("解放済み"), unreadable: L("読み取り不能"),
}
function jobReason(job) {
  switch (job.state) {
    case 'missing': return L("ジャーナルへの初回書き込み前に転送が停止しました。")
    case 'uploading': return L("入力は配置済みですが、ジョブは送信されていません。")
    case 'queued': return L("待機中です。実行はまだ開始されていません。")
    case 'running':
    case 'cancelling': {
      const pid = job.reason.match(/supervisor pid (\d+)\)/)?.[1]
      return pid ? L("スーパーバイザー PID {0} が処理しています。", pid)
        : L("スーパーバイザーがなく、リスナーが中断として記録します。")
    }
    case 'succeeded': return L("結果が回収されていません。")
    case 'failed':
    case 'cancelled':
    case 'interrupted': return L("入力、ログ、途中結果が残っています。")
    case 'released': return L("解放済みです。リスナーが残りの削除を完了します。")
    default: return job.reason
  }
}

const selected = new Set()
const maxJobs = 256
let send, jobsSnapshot, keysSnapshot, review, busy = false, refreshing = false
const formatBytes = new Intl.NumberFormat(locale, { maximumFractionDigits: 1 })
function size(bytes) {
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']
  let i = 0
  while (bytes >= 1024 && i < units.length - 1) { bytes /= 1024; i++ }
  return formatBytes.format(bytes) + ' ' + units[i]
}
function element(tag, text) {
  const node = document.createElement(tag)
  if (text !== undefined) node.textContent = text
  return node
}
function report(text, error = false) {
  $('feedback').textContent = text
  $('feedback').toggleAttribute('data-error', error)
}
function controls() {
  $('review-jobs').disabled = busy || !!review || !selected.size
  $('submit-key').disabled = busy || !!review || !keysSnapshot
  for (const input of $('jobs').querySelectorAll('input'))
    input.disabled = busy || !!review || (!input.checked && selected.size >= maxJobs)
  for (const button of $('keys').querySelectorAll('button')) button.disabled = busy || !!review
}
function renderJobs(data) {
  const names = new Set(data.jobs.map(job => job.project_scope + '/' + job.job_id))
  for (const name of selected) if (!names.has(name)) selected.delete(name)
  const rows = data.jobs.map(job => {
    const name = job.project_scope + '/' + job.job_id
    const tr = element('tr')
    const cell = value => {
      const td = element('td')
      td.append(value)
      tr.append(td)
      return td
    }
    const input = element('input')
    input.type = 'checkbox'
    input.checked = selected.has(name)
    input.setAttribute('aria-label', L("ジョブ {0} を選択", name))
    input.addEventListener('change', () => {
      if (input.checked) selected.add(name)
      else selected.delete(name)
      controls()
    })
    cell(input)
    for (const id of [job.project_scope, job.job_id]) {
      const code = element('code', id.slice(0, 12))
      code.title = id
      cell(code)
    }
    cell(states[job.state] ?? job.state)
    cell(size(job.bytes)).className = 'number'
    const time = element('time', job.last_written.replace('T', ' ').replace('Z', ''))
    time.dateTime = job.last_written
    cell(time)
    cell(jobReason(job))
    return tr
  })
  $('jobs').replaceChildren(...rows)
  $('empty-jobs').hidden = !!rows.length
  controls()
}
function renderKeys(data) {
  const entries = data.keys.map(key => {
    const line = element('li')
    const label = element('span')
    label.append(element('code', key))
    if (key === data.current) label.append(' ', element('small', L("このホスト")))
    const remove = element('button', L("削除"))
    remove.type = 'button'
    remove.addEventListener('click', () => ask({
      operation: 'RemoveKey', body: { key },
      text: key === data.current
        ? L("このホストの鍵を削除しますか？ 再接続にはワーカー側での許可が必要です。")
        : L("ホスト鍵 {0} を削除しますか？", key),
    }))
    line.append(label, remove)
    return line
  })
  $('keys').replaceChildren(...entries)
  controls()
}
async function request(operation, body) {
  const raw = await send(operation, body ? JSON.stringify(body) : '')
  const data = JSON.parse(raw)
  if (data.error) throw new Error(data.error)
  return { raw, data }
}
async function refresh() {
  if (busy || refreshing) return
  refreshing = true
  const results = await Promise.allSettled([request('Jobs'), request('Keys')])
  refreshing = false
  if (busy) return
  const [jobs, keys] = results
  if (jobs.status === 'fulfilled' && jobs.value.raw !== jobsSnapshot) {
    jobsSnapshot = jobs.value.raw
    renderJobs(jobs.value.data)
  }
  if (keys.status === 'fulfilled' && keys.value.raw !== keysSnapshot) {
    keysSnapshot = keys.value.raw
    renderKeys(keys.value.data)
  }
  const failure = results.find(result => result.status === 'rejected')
  if (failure) report(L("ワーカーの情報を取得できません: {0}", failure.reason.message), true)
  else if ($('feedback').dataset.refreshError === '1') report('')
  $('feedback').dataset.refreshError = failure ? '1' : ''
}
function ask(next) {
  review = next
  $('review-text').textContent = next.text
  $('active-label').hidden = next.operation !== 'RemoveJobs'
  $('active').checked = false
  $('review').hidden = false
  $('cancel').focus()
  controls()
}
function closeReview() {
  review = undefined
  $('review').hidden = true
  controls()
}
async function change(operation, body) {
  if (busy) return
  busy = true
  controls()
  report(L("処理中…"))
  $('feedback').dataset.refreshError = ''
  try {
    const { data: { token } } = await request('Challenge')
    const { data } = await request(operation, { ...body, token })
    if (operation === 'RemoveJobs') {
      const failures = []
      let removed = 0
      for (const row of data.results) {
        if (row.status === 'removed' || row.status === 'released') {
          selected.delete(row.job)
          removed++
        } else {
          const detail = row.status === 'active' ? L("実行状態のジョブの削除には明示的な確認が必要です。")
            : row.status === 'supervised' ? L("スーパーバイザーが動いています。先に実行をキャンセルしてください。")
            : row.detail
          failures.push(row.job.slice(65, 77) + ' · ' + detail)
        }
      }
      report([L("{0} 件のジョブを解放しました。", removed), ...failures].join(' '), !!failures.length)
    } else if (operation === 'AddKey') {
      $('public-key').value = ''
      report(L("ホスト鍵を追加しました。"))
    } else {
      report(L("ホスト鍵を削除しました。"))
    }
  } catch (e) {
    report(L("変更を完了できませんでした: {0}", e.message), true)
  } finally {
    busy = false
    controls()
    await refresh()
  }
}
$('review-jobs').addEventListener('click', () => ask({
  operation: 'RemoveJobs', body: { jobs: [...selected] },
  text: L("選択した {0} 件のジョブのデータをワーカーから削除しますか？", selected.size),
}))
$('cancel').addEventListener('click', closeReview)
$('confirm').addEventListener('click', () => {
  if (!review) return
  const { operation, body } = review
  if (operation === 'RemoveJobs') body.active = $('active').checked
  closeReview()
  void change(operation, body)
})
$('add-key').addEventListener('submit', event => {
  event.preventDefault()
  const key = $('public-key').value.trim()
  if (key && !review && keysSnapshot) void change('AddKey', { key })
})
function start(exchange) {
  send = exchange
  void refresh()
  setInterval(refresh, 5000)
}
const native = window.webkit?.messageHandlers?.workerDashboard
if (native) {
  start((operation, body) => native.postMessage({ operation, body }))
} else if (window !== parent) {
  function attach(event) {
    if (event.source !== parent || event.data !== 'tsugumi-worker-dashboard' || event.ports.length !== 1) return
    removeEventListener('message', attach)
    const [port] = event.ports
    const pending = new Map()
    let sequence = 0
    port.onmessage = ({ data }) => {
      const reply = pending.get(data.id)
      if (!reply) return
      pending.delete(data.id)
      if (data.error) reply.reject(new Error(data.error))
      else reply.resolve(data.body)
    }
    start((operation, body) => new Promise((resolve, reject) => {
      const id = ++sequence
      pending.set(id, { resolve, reject })
      port.postMessage({ id, operation, body })
    }))
  }
  addEventListener('message', attach)
}
