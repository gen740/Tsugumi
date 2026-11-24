import { L } from '../i18n'
import { toggleWorkerBit, workerBits } from '../../shared/workerBits'
import { Select } from './controls'

export function WorkerBitsEditor({ bits, onChange, disabled }: {
  bits: number[]
  onChange: (bits: number[]) => void
  disabled?: boolean
}) {
  return <div className="row gap8 center">
    {bits.length > 0 && <span className="mono selectable">{bits.join(', ')}</span>}
    <Select value="" disabled={disabled} onChange={value => onChange(toggleWorkerBit(bits, Number(value)))}
      options={[{ value: '', label: L('ビットを切り替える…'), disabled: true },
        ...workerBits.map(bit => ({ value: String(bit), label: bits.includes(bit) ? `✓ ${bit}` : String(bit) }))]} />
  </div>
}
