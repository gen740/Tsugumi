import type { AudioChunk, AudioRequest } from './workerProtocol'

declare const currentFrame: number
declare class AudioWorkletProcessor {
  readonly port: MessagePort
}
declare function registerProcessor(name: string, processor: typeof AudioWorkletProcessor): void

// Request chunks ahead of the audio thread's playhead so worker and port jitter
// fits inside the lookahead. Requests are capped while a view is unresponsive.
const framesPerChunk = 768
const leadFrames = 1024
const maxPending = 3

class ViewAudio extends AudioWorkletProcessor {
  private worker?: MessagePort
  private nextFrame = 0
  private readonly pending = new Set<number>()
  private readonly chunks = new Map<number, Float32Array>()

  constructor() {
    super()
    this.port.onmessage = ({ data }: MessageEvent<MessagePort>) => {
      this.worker = data
      this.worker.onmessage = ({ data: chunk }: MessageEvent<AudioChunk>) => {
        if (!this.pending.delete(chunk.frame)) return
        if (chunk.samples && chunk.frame + framesPerChunk > currentFrame)
          this.chunks.set(chunk.frame, chunk.samples)
      }
    }
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]) {
    const [left, right] = outputs[0]
    const end = currentFrame + left.length
    if (this.worker && this.pending.size < maxPending && this.nextFrame < end + leadFrames) {
      const frame = this.nextFrame < currentFrame ? currentFrame + leadFrames : this.nextFrame
      this.nextFrame = frame + framesPerChunk
      this.pending.add(frame)
      const request: AudioRequest = { frame, frames: framesPerChunk }
      this.worker.postMessage(request)
    }
    for (const [start, samples] of this.chunks) {
      const limit = start + framesPerChunk
      for (let frame = Math.max(start, currentFrame); frame < Math.min(end, limit); ++frame) {
        left[frame - currentFrame] = samples[2 * (frame - start)]
        right[frame - currentFrame] = samples[2 * (frame - start) + 1]
      }
      if (limit <= end) this.chunks.delete(start)
    }
    return true
  }
}

registerProcessor('tsugumi-view-audio', ViewAudio)
