import { DawnViewInstance } from './instance'
import { loadView } from './loadView'
import { routeKey, sampleRate, type AudioChunk, type AudioClock, type AudioRequest, type ViewCommand, type ViewNotice } from './workerProtocol'

const worker = globalThis as unknown as {
  postMessage(notice: ViewNotice, transfer?: Transferable[]): void
  onmessage: (event: MessageEvent<ViewCommand>) => void
}
const post = (notice: ViewNotice, transfer?: Transferable[]) => worker.postMessage(notice, transfer)
const errorText = (cause: unknown) => cause instanceof Error ? cause.message : String(cause)

const routes = new Set<string>()
let instance: DawnViewInstance | undefined
let visible = true
let clock: AudioClock | undefined
let nextSend = 0
let sendReply: ((error?: string) => void) | undefined

function onAudio(port: MessagePort, initialClock: AudioClock) {
  clock = initialClock
  port.onmessage = ({ data }: MessageEvent<AudioRequest>) => {
    const { frame, frames } = data
    const heard = clock!.absoluteTime + (frame / sampleRate - clock!.contextTime) * 1000
    const samples = instance?.audio(frames, heard)
    const chunk: AudioChunk = { frame, samples }
    port.postMessage(chunk, samples ? [samples.buffer] : [])
  }
}

async function run(command: ViewCommand) {
  switch (command.type) {
    case 'start': {
      for (const route of command.routes) routes.add(route)
      visible = command.visible
      const { module } = await loadView(command.bytes)
      instance = await DawnViewInstance.mount(module, command.canvas, command.size, {
        send: (bytes, route) => new Promise<void>((resolve, reject) => {
          const id = ++nextSend
          sendReply = error => error ? reject(new Error(error)) : resolve()
          post({ type: 'send', id, bytes, route }, [bytes.buffer, route.buffer])
        }),
        acceptsRoute: route => route.every(byte => byte === 0) || routes.has(routeKey(route)),
        onSendError: cause => post({ type: 'sendError', message: cause === undefined ? '' : errorText(cause) }),
        onGpuError: message => post({ type: 'gpuError', message }),
        onFatal: cause => post({ type: 'fatal', message: errorText(cause) }),
        onCall: started => post({ type: 'call', started }),
        onTextArea: area => post({ type: 'textArea', area }),
      }, command.refreshPeriod, visible)
      post({ type: 'ready', features: instance.features })
      break
    }
    case 'route':
      routes.add(command.route)
      break
    case 'visible':
      visible = command.visible
      instance?.setVisible(visible)
      break
    case 'refresh':
      if (instance) instance.refreshPeriod = command.period
      break
    case 'message':
      instance!.deliver(command.bytes, command.at)
      post({ type: 'complete', id: command.id })
      break
    case 'resize':
      instance!.resize(command.size, command.at)
      post({ type: 'complete', id: command.id })
      break
    case 'input':
      instance!.input(command.event, command.at)
      post({ type: 'complete', id: command.id })
      break
    case 'snapshot': {
      const png = await instance!.snapshot()
      post({ type: 'complete', id: command.id, png }, [png.buffer])
      break
    }
    case 'sendResult':
      if (command.id === nextSend) {
        sendReply?.(command.error)
        sendReply = undefined
      }
      break
    case 'audio':
      onAudio(command.port, command.clock)
      break
    case 'audioClock':
      clock = command.clock
      break
  }
}

worker.onmessage = ({ data }: MessageEvent<ViewCommand>) => {
  void run(data).catch(cause => {
    const message = errorText(cause)
    if (data.type === 'message' || data.type === 'resize' || data.type === 'input' || data.type === 'snapshot')
      post({ type: 'complete', id: data.id, error: message })
    else
      post({ type: 'fatal', message })
  })
}
