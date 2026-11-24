import { sampleRate, type AudioClock } from './workerProtocol'

export class ViewAudio {
  private readonly context: AudioContext
  private timer?: number
  private disposed = false

  constructor(
    private readonly onSuspended: (suspended: boolean) => void,
    private readonly connect: (port: MessagePort, clock: AudioClock) => void,
    private readonly sync: (clock: AudioClock) => void,
  ) {
    this.context = new AudioContext({ sampleRate })
    this.context.onstatechange = () => {
      this.reportState()
      if (this.context.state === 'running') this.sync(this.clock())
    }
    this.reportState()
  }

  private reportState() {
    if (!this.disposed) this.onSuspended(this.context.state === 'suspended')
  }

  enable() {
    if (this.context.state === 'suspended') this.sync(this.latencyClock())
    return this.context.resume()
  }

  private clock(): AudioClock {
    const context = this.context
    const stamp = context.getOutputTimestamp()
    if (stamp.performanceTime)
      return { contextTime: stamp.contextTime!, absoluteTime: performance.timeOrigin + stamp.performanceTime }
    return this.latencyClock()
  }

  private latencyClock(): AudioClock {
    const context = this.context
    const clock = performance
    return { contextTime: context.currentTime, absoluteTime: clock.timeOrigin + clock.now()
      + (context.baseLatency + (context.outputLatency || 0)) * 1000 }
  }

  async start() {
    await this.context.audioWorklet.addModule(new URL('./dawn-view-audio.js', import.meta.url).href)
    if (this.disposed) return
    const node = new AudioWorkletNode(this.context, 'tsugumi-view-audio', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
    })
    const channel = new MessageChannel()
    this.connect(channel.port1, this.clock())
    node.port.postMessage(channel.port2, [channel.port2])
    node.connect(this.context.destination)
    this.timer = window.setInterval(() => {
      if (this.context.state === 'running') this.sync(this.clock())
    }, 200)
  }

  dispose() {
    this.disposed = true
    window.clearInterval(this.timer)
    this.context.onstatechange = null
    void this.context.close()
  }
}
