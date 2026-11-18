// Presentation types for host-owned execution service registrations.
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
  workerPublicKey: string
  heartbeatIntervalSeconds: number
  missedHeartbeats: number
  grade: string
  capabilities: ExecutionServiceCapabilities
}

export interface ExecutionServiceRegistration {
  id: string
  name: string
  endpoint: string
  workerPublicKey: string
  heartbeatIntervalSeconds: number
  missedHeartbeats: number
  grade: string
}

export interface ExecutionServicesState { services: ExecutionServiceConnection[]; hostPublicKey: string }
