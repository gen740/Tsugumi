import assert from 'node:assert/strict'
import { ProjectSession, DefinitionTemplate, templateIcon } from '../src/model/session'
import { get, parse } from '../src/shared/json'

for (const engine of ['rmarkdown', 'quarto']) {
  const session = new ProjectSession('/report-test', 'report-test', 'report-test')
  session.project = parse('{"name":"report-test","canvas":{"cards":[],"connections":[]}}')
  session.layout = parse('{}')
  session.scheduleSave = () => {}
  const template = `report_${engine}` as DefinitionTemplate
  const card = session.addCard(template, [], { x: 0, y: 0 })
  const definition = session.cardDefinition(card)
  assert.equal(get(definition, 'language'), 'r')
  assert.equal(get(definition, 'retainArtifact'), true)
  assert.equal(get(definition, 'artifactOutput'), 'report')
  assert.ok(templateIcon(template))
  assert.ok(session.sourcePaths.some(p => p.endsWith(engine === 'quarto' ? 'report.qmd' : 'report.Rmd')))
  assert.ok(session.sourcePaths.some(p => p.endsWith('_outputs.fbs')))
}
console.log('report Card creation: passed')
