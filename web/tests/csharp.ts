import assert from 'node:assert/strict'
import { templateIcon, TEMPLATES } from '../src/model/session'
assert.ok(templateIcon('csharp'))
assert.ok(TEMPLATES.some(t => t.id === 'csharp'))
