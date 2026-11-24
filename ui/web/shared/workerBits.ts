export const workerBits = Array.from({ length: 64 }, (_, bit) => bit)

export function validWorkerBits(bits: number[]): boolean {
  return new Set(bits).size === bits.length && bits.every(bit => Number.isInteger(bit) && bit >= 0 && bit < 64)
}

export function toggleWorkerBit(bits: number[], bit: number): number[] {
  return bits.includes(bit) ? bits.filter(x => x !== bit) : [...bits, bit].sort((a, b) => a - b)
}
