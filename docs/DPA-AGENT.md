# DPA external desktop assistant

This repository owns future DPA intelligence development. It retains the pi/Electron
desktop shell and receives curated scientific and bridge-client source. See
[migration status](migration/HANDOFF-20261026.md), [governance](GOVERNANCE.md) and
[source-license provenance](../NOTICE.md).

## Setup against the current DPA bridge

Use Node 24. Configure pi/model login and install pi-mcp-adapter according to their
upstream instructions. From this repository, run `scripts/setup-dpa-agent.ps1` on
Windows. It builds/checks the local read-only MCP package and writes configuration
into an independent `.dpa-workspace` by default. Existing configs/instructions are
preserved. It does not install persona files into the DPA source tree or edit global
MCP configuration. `-AgentWorkspace` selects a separate desired workspace;
`-DpaWorkspace` is a compatibility alias for that parameter.

Install/run the desktop separately with `npm install` and `npm run dev`, then open
the Agent workspace. Start DPA manually. Transition builds require
`DPA_ENABLE_AGENT_BRIDGE=1` to load the compatibility service. The live bridge
manifest is `%LOCALAPPDATA%/DPA-2026/mcp/bridge.json`. No token is stored in Git.

The root `.mcp.json` is a relative-path example for using this checkout as the
workspace; setup generates absolute executable/server paths in the chosen workspace.
Client build command: `npm run build --prefix packages/dpa-mcp-server`.

## First use and troubleshooting

Create a fresh Agent session and inspect the actual MCP tool list. Current native
tools use names such as `dpa.project.current`; only tools explicitly classified `read`
are exposed. If tools are absent, check DPA is running, the manifest exists, the MCP
client was built, and pi-mcp-adapter is enabled; then reconnect/new-session.

The previous docs assumed an ExternalAgentBridge under AIAssistant, a server at
tools/dpa-mcp/server.js and dpa_* write/auto-launch tools. Those are a different
protocol, not capabilities verified against this DPA baseline. Do not translate
names, guess UI actions, switch to writable tokens or bypass native confirmation.
Read-only operation is the current supported integration boundary.

Automated MCP checks use a simulated bridge. Live DPA, Electron startup, real models
and physical devices each need separate acceptance. Source transfer is not proof of
these runtime outcomes.
