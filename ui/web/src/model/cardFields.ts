import { JObj, num, obj } from '../../shared/json'

export const recordSettings = ['project', 'on', 'off'] as const
/** Values written when converting a Draft in the editor. */
export function implementedCard(kind: string, symbol: string, system: string): JObj {
  const definition = obj([['type', kind], ['symbol', symbol]])
  if (['code', 'input', 'merge', 'collect'].includes(kind)) {
    definition.set('maxRuns', num(1))
    definition.set('recordIO', 'project')
  }
  if (kind === 'merge') definition.set('inputs', [obj([['name', 'a']]), obj([['name', 'b']])])
  if (kind === 'code' || kind === 'input') {
    definition.set('inputs', [])
    definition.set('reuseWorker', false)
    definition.set('timeoutSeconds', num(0))
    definition.set('buildTimeoutSeconds', num(0))
    definition.set('recordLogs', 'project')
    definition.set('sandbox', obj([['filesystem', true], ['pid', true], ['ipc', true], ['identity', true],
      ['limits', obj([['cpu', num(0)], ['memoryMiB', num(0)]])]]))
    definition.set('grants', obj([['network', false], ['gui', false]]))
    definition.set('system', system)
    definition.set('workerCapabilities', [])
  }
  return definition
}
