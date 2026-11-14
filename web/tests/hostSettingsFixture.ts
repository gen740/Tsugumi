import { CoreAgentHost } from '../server/coreAgent'
/** A typed Host settings reply for presentation tests without a daemon. */
export function mockHostSettings() {
  const environment = CoreAgentHost.prototype.provisionEnvironment
  CoreAgentHost.prototype.provisionEnvironment = async () => {}
  const original = CoreAgentHost.prototype.getSettings
  CoreAgentHost.prototype.getSettings = async () => ({ revision: 0, values: Object.entries({
    'agent.provider': 'anthropic', 'agent.model': 'claude-sonnet-5-5', 'agent.model.openai': 'gpt-6.1-sol', 'agent.model.chatgpt': 'gpt-6.1-sol',
    'agent.fixturePort': 8765, 'agent.effort': '', 'agent.fastModel': '', 'agent.webSearch': false, 'agent.permissions.allowed': [],
    'agent.skillFolders': [], 'versions.noteMode': 'mechanical', 'app.language': 'ja',
  }).map(([key, value]) => ({ key, value_json: JSON.stringify(value) })) })
  return () => { CoreAgentHost.prototype.getSettings = original; CoreAgentHost.prototype.provisionEnvironment = environment }
}
