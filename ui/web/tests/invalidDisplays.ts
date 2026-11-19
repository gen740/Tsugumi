import assert from 'node:assert/strict'
import { D } from '../src/wire/rpc'
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

console.log('display presentation: explicit unavailable state passed')
