# DPA read-only MCP client

Curated from the preserved DPA intelligence baseline. Source receipts and license
status are in ../../docs/migration/source-manifest-20261026.json and ../../NOTICE.md.

Requires Node 24. Install from this directory with `npm ci --ignore-scripts`, then
`npm run self-check`. Run with `node dist/index.js` using MCP stdio. DPA must already
be running. The client reads `%LOCALAPPDATA%/DPA-2026/mcp/bridge.json`; override with
`DPA_MCP_BRIDGE` for an explicitly selected bridge manifest.

Only the manifest's read_token and live read-only tools are used. Missing bridge or
catalog exposes no tools. Endpoint origin and HTTP redirects are constrained to
avoid forwarding credentials. No auto-launch, write-token fallback or dpa_* tool
aliases are provided. Native DPA remains the operation authorization authority.
