import { layoutHost } from '../src/shared/hostLayout'
layoutHost.validate = async (json) => json
// UI-only tests replace the read-only host boundary, as they replace the core.
// Real placement and transaction behavior run against hostd in test_card_placement.py.
layoutHost.complete = async (_project, layout) => layout
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
const root = process.argv[2]
// Agent recovery, images, references, approvals and compaction run through
// protocol/agent.fbs in tests/core/agent_rpc_test.cc. These are UI behaviors.

await import("./clientViewState")
await import("./hostRules")
await import("./serviceProtocol")
await import("./agentRequests")
await import('./chatModelRefresh')
await import('./chatFailure')
await import('./scopedSkills')
await import("./guideRpc")
await import("./chatGPTSetup")
await import("./environmentHost")
await import("./hostReadiness")
await import("./chatStopped")
await import("./chatInterrupted")
await import("./folderAttachments")
await import("./switchCard")
await import("./csharp")
await import("./reports")

await import("./table")
await import("./artifactHTML")

await import("./draft")
await import("./comments")
await import("./draftWires")
await import("./freedName")
await import("./summary")
await import("./executionSettings")
await import("./executionLists")
await import("./versions")
await import("./wires")
await import("./wireDemos")

await import("./modelMenu")
await import('./agentRealCheck')
await import("./settings")
await import("./providerKeys")
await import('./thoughts')

await import("./chatFollow")
await import("./terminalText")

await import("./projectActions")

await import("./thumbnails")

await import("./liveCode")

await import("./helperBirds")
await import("./presence")
await import("./leaveProject")
await import("./terminalInput")

await import("./portGrid")

await import("./marquee")


await import("./freeSpot")



await import("./draftDisplays")

await import("./nixSources")
await import("./marimoTemplate")

await import("./sessionRestore")
await import("./sessionConnect")
await import("./projectTrust")

await import("./perchScale")
await import("./demoReplay")
await import("./visualizationTemplates")
await import("./evidence")
await import('./environments')

await import('./runComparison')

await import('./moduleThumbnails')
await import('./moduleViewer')
await import('./wasmView.mjs')
await import('./figurePreview')
await import('./nixReferences')
await import('./nixReferenceOverlay')

await import('./ruby')

await import("./lineChart")

await import("./runScratch")
await import('./cardIdentity')
await import('./agentAppearance')

await import("./invalidDisplays")




await import('./marimo')

await import('./projectOperations')
await import('./reviewOperations')
