import assert from 'node:assert/strict'
import { TEMPLATES } from '../src/model/session'
import { visualizationTemplates } from '../src/model/visualizationTemplates'
import { builtinCardItems } from '../src/ui/canvas'

const offered: string[] = []
const visit = (items: ReturnType<typeof builtinCardItems>) => {
  for (const item of items) if (item && typeof item !== 'string') {
    item.action?.()
    if (item.submenu) visit(item.submenu)
  }
}
visit(builtinCardItems(id => offered.push(id)))
assert.equal(visualizationTemplates.length, 3)
for (const template of visualizationTemplates) {
  assert.ok(TEMPLATES.some(t => t.id === template.id))
  assert.ok(offered.includes(template.id))
}
