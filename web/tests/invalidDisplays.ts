import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import { D, unpackDisplay } from '../src/shared/rpc'
import { recordedDisplay, displaySummary } from '../src/model/session'

const snapshot = (type: D.DisplayValue, value: D.DisplayT['value']) =>
  recordedDisplay(Object.assign(new D.DisplayT(), { valueType: type, value }))!
for (const value of [NaN, Infinity, -Infinity]) {
  const display = snapshot(D.DisplayValue.DisplayScalar, new D.DisplayScalarT(value, 'value'))
  assert.equal(display.value.kind, 'unavailable')
  assert.ok(displaySummary(display))
}
assert.equal(snapshot(D.DisplayValue.DisplayTable, new D.DisplayTableT([], [])).value.kind, 'unavailable')
assert.equal(snapshot(D.DisplayValue.DisplayTable, new D.DisplayTableT(['one'], [new D.TableRowT(['one', 'two'])])).value.kind, 'unavailable')
assert.equal(snapshot(D.DisplayValue.DisplayImage, new D.DisplayImageT('image/png', [1,2,3], '')).value.kind, 'unavailable')
assert.equal(snapshot(D.DisplayValue.DisplayPointCloud, new D.DisplayPointCloudT([0], [], [0], [])).value.kind, 'unavailable')
assert.equal(snapshot(D.DisplayValue.NONE, null).value.kind, 'unavailable')
const identity = '/nix/store/00000000000000000000000000000000-view'
assert.equal(snapshot(D.DisplayValue.DisplayModule, new D.DisplayModuleT(identity, '{', [])).value.kind, 'unavailable')
assert.equal(snapshot(D.DisplayValue.DisplayModule, new D.DisplayModuleT(identity, '{}', [new D.DisplayBlobT('a',[1]),new D.DisplayBlobT('a',[2])])).value.kind, 'unavailable')
assert.equal(snapshot(D.DisplayValue.DisplayText, new D.DisplayTextT('hello')).value.kind, 'text')

// The object API would replace invalid UTF-8 before the renderer could detect it.
const b = new Builder()
const text = D.DisplayText.createDisplayText(b, b.createString(new Uint8Array([255])), false)
const card = new D.CardPathT(['card']).pack(b)
const run = b.createString('run')
D.Display.startDisplay(b)
D.Display.addCard(b, card)
D.Display.addRunId(b, run)
D.Display.addValueType(b, D.DisplayValue.DisplayText)
D.Display.addValue(b, text)
b.finish(D.Display.endDisplay(b))
const display = unpackDisplay(D.Display.getRootAsDisplay(new ByteBuffer(b.asUint8Array())))
assert.equal(recordedDisplay(display)!.value.kind, 'unavailable')
console.log('invalid displays: explicit unavailable state passed')

// Shared references fit a tiny wire frame but expand beyond the display budget.
{
  const b = new Builder()
  const cell = b.createString('x'.repeat(4096))
  const row = D.TableRow.createTableRow(b, D.TableRow.createCellsVector(b, [cell]))
  const rows = D.DisplayTable.createRowsVector(b, Array(5000).fill(row))
  const columns = D.DisplayTable.createColumnsVector(b, [b.createString('value')])
  const table = D.DisplayTable.createDisplayTable(b, columns, rows)
  const card = new D.CardPathT(['card']).pack(b), run = b.createString('run')
  D.Display.startDisplay(b)
  D.Display.addCard(b, card); D.Display.addRunId(b, run)
  D.Display.addValueType(b, D.DisplayValue.DisplayTable); D.Display.addValue(b, table)
  b.finish(D.Display.endDisplay(b))
  assert.ok(b.asUint8Array().length < 65536)
  assert.equal(recordedDisplay(unpackDisplay(D.Display.getRootAsDisplay(new ByteBuffer(b.asUint8Array()))))!.value.kind, 'unavailable')
}
