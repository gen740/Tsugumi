// Presentation types for core-owned execution services.
export interface ExecutionServiceCapabilities {
  serverId: string
  nativeSystem: string
  systems: string[]
  isolation: 'pid_namespace'
  resourceLimits: boolean
  maxConcurrentJobs: number
  workerContract: number
}

export interface ExecutionServiceConnection {
  id: string
  name: string
  endpoint: string
  capabilities: ExecutionServiceCapabilities
}

export interface ExecutionServiceRegistration {
  id: string
  name: string
  endpoint: string
  token?: string
}

