#!/usr/bin/env node
import { readFile } from "node:fs/promises";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { CallToolRequestSchema, ListToolsRequestSchema } from "@modelcontextprotocol/sdk/types.js";

type JsonObject = Record<string, unknown>;

type BridgeManifest = {
  host: string;
  port: number;
  read_token: string;
  endpoints?: {
    health?: string;
    tools?: string;
    call?: string;
  };
};

type DpaTool = {
  name: string;
  title?: string;
  description?: string;
  inputSchema?: JsonObject;
  outputSchema?: JsonObject;
  dangerLevel?: string;
};

const TIMEOUT_MS = Number(process.env.DPA_MCP_TIMEOUT_MS) || 120_000;

export function defaultManifestPath(): string {
  if (process.env.DPA_MCP_BRIDGE) return process.env.DPA_MCP_BRIDGE;
  const base = process.env.LOCALAPPDATA ?? path.join(process.env.HOME ?? ".", ".local", "share");
  return path.join(base, "DPA-2026", "mcp", "bridge.json");
}

export async function readManifest(manifestPath = defaultManifestPath()): Promise<BridgeManifest> {
  const manifest = JSON.parse(await readFile(manifestPath, "utf8")) as Partial<BridgeManifest>;
  if (manifest.host !== "127.0.0.1" || typeof manifest.port !== "number" ||
      !Number.isInteger(manifest.port) || manifest.port < 1 || manifest.port > 65_535 ||
      typeof manifest.read_token !== "string" || !manifest.read_token) {
    throw new Error("Invalid DPA bridge manifest.");
  }
  return manifest as BridgeManifest;
}

function bridgeUrl(manifest: BridgeManifest, endpoint: keyof NonNullable<BridgeManifest["endpoints"]>): string {
  const route = manifest.endpoints?.[endpoint] ?? `/${endpoint}`;
  const base = new URL(`http://${manifest.host}:${manifest.port}`);
  const url = new URL(route, base);
  if (url.origin !== base.origin) throw new Error("DPA bridge endpoint must stay on loopback.");
  return url.toString();
}

async function bridgeFetch(manifest: BridgeManifest, endpoint: "tools" | "call", init?: RequestInit): Promise<JsonObject> {
  const response = await fetch(bridgeUrl(manifest, endpoint), {
    ...init,
    redirect: "error",
    signal: AbortSignal.timeout(TIMEOUT_MS),
    headers: {
      authorization: `Bearer ${manifest.read_token}`,
      "content-type": "application/json",
      ...(init?.headers ?? {})
    }
  });
  const text = await response.text();
  const body = text ? JSON.parse(text) as JsonObject : {};
  if (!response.ok) throw new Error(String(body.error ?? response.statusText));
  return body;
}

export async function listDpaTools(manifestPath = defaultManifestPath()): Promise<DpaTool[]> {
  try {
    const manifest = await readManifest(manifestPath);
    const body = await bridgeFetch(manifest, "tools");
    if (!Array.isArray(body.tools)) return [];
    return (body.tools as DpaTool[]).filter((tool) =>
      typeof tool?.name === "string" && tool.name.length > 0 && tool.dangerLevel === "read");
  } catch {
    return [];
  }
}

function mcpTool(tool: DpaTool): JsonObject {
  return {
    name: tool.name,
    title: tool.title ?? tool.name,
    description: tool.description ?? tool.title ?? tool.name,
    inputSchema: tool.inputSchema ?? { type: "object", properties: {}, required: [] },
    ...(tool.outputSchema ? { outputSchema: tool.outputSchema } : {})
  };
}

function errorResult(message: string): JsonObject {
  return {
    isError: true,
    content: [{ type: "text", text: message }],
    structuredContent: { ok: false, error: message }
  };
}

export async function callDpaTool(name: string, args: JsonObject = {}, manifestPath = defaultManifestPath()): Promise<JsonObject> {
  let manifest: BridgeManifest;
  try {
    manifest = await readManifest(manifestPath);
  } catch {
    return errorResult(`DPA desktop bridge is not running. Start DPA first, then retry ${name}.`);
  }
  try {
    const tools = await listDpaTools(manifestPath);
    if (!tools.some((tool) => tool.name === name)) {
      return errorResult("Tool is not available in the live read-only catalog.");
    }
    const result = await bridgeFetch(manifest, "call", {
      method: "POST",
      body: JSON.stringify({ name, arguments: args })
    });
    const ok = result.ok !== false;
    const summary = typeof result.summary === "string" && result.summary ? result.summary : ok ? "DPA tool completed." : String(result.error ?? "DPA tool failed.");
    return {
      isError: !ok,
      content: [{ type: "text", text: summary }],
      structuredContent: result
    };
  } catch (error) {
    const msg = error instanceof Error ? error.message : String(error);
    // The common failure is a stale manifest whose bridge is gone: readManifest succeeds
    // but the fetch fails at the socket. Give the same clear "start DPA" guidance.
    if (/fetch failed|ECONNREFUSED|ECONNRESET|ETIMEDOUT|network|socket|aborted|timeout/i.test(msg))
      return errorResult(`DPA desktop bridge is not reachable. Start DPA (and keep it running), then retry ${name}.`);
    return errorResult(msg);
  }
}

export async function createServer(): Promise<Server> {
  const server = new Server({ name: "dpa-mcp-server", version: "0.2.0" }, { capabilities: { tools: {} } });
  server.setRequestHandler(ListToolsRequestSchema, async () => ({
    tools: (await listDpaTools()).map(mcpTool) as any
  }));
  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const params = request.params;
    return await callDpaTool(params.name, (params.arguments ?? {}) as JsonObject) as any;
  });
  return server;
}

async function main(): Promise<void> {
  const server = await createServer();
  await server.connect(new StdioServerTransport());
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error) => {
    console.error(error);
    process.exit(1);
  });
}
