import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { callDpaTool, listDpaTools } from "./index.js";

const tmp = await mkdtemp(path.join(os.tmpdir(), "dpa-mcp-"));
const readToken = "test-read-token";
let requestCount = 0;
let callCount = 0;

const server = http.createServer((req, res) => {
  requestCount += 1;
  if (req.headers.authorization !== `Bearer ${readToken}`) {
    res.writeHead(401, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: false, error: "unauthorized" }));
    return;
  }
  if (req.method === "GET" && req.url === "/tools") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: true, tools: [
      { name: "dpa.project.current", title: "Get context", dangerLevel: "read", inputSchema: { type: "object", properties: {} } },
      { name: "dpa.project.save", title: "Save project", dangerLevel: "write", inputSchema: { type: "object", properties: {} } }
    ] }));
    return;
  }
  if (req.method === "POST" && req.url === "/call") {
    callCount += 1;
    req.resume();
    req.on("end", () => {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, summary: "ok", data: { current: true } }));
    });
    return;
  }
  if (req.url === "/redirect") {
    res.writeHead(302, { location: "/tools" });
    res.end();
    return;
  }
  res.writeHead(404, { "content-type": "application/json" });
  res.end(JSON.stringify({ ok: false, error: "not_found" }));
});

await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
const address = server.address();
assert(address && typeof address === "object");
const manifestPath = path.join(tmp, "bridge.json");
await writeFile(manifestPath, JSON.stringify({ host: "127.0.0.1", port: address.port, read_token: readToken, endpoints: { tools: "/tools", call: "/call" } }));

try {
  const tools = await listDpaTools(manifestPath);
  assert.deepEqual(tools.map((tool) => tool.name), ["dpa.project.current"]);
  const result = await callDpaTool("dpa.project.current", {}, manifestPath);
  assert.equal(result.isError, false);
  assert.deepEqual((result.structuredContent as any).data, { current: true });

  const beforeWrite = callCount;
  assert.equal((await callDpaTool("dpa.project.save", {}, manifestPath)).isError, true);
  assert.equal((await callDpaTool("unlisted.tool", {}, manifestPath)).isError, true);
  assert.equal(callCount, beforeWrite);

  const redirectManifest = path.join(tmp, "redirect.json");
  await writeFile(redirectManifest, JSON.stringify({
    host: "127.0.0.1", port: address.port, read_token: readToken,
    endpoints: { tools: "/redirect" }
  }));
  const beforeRedirect = requestCount;
  assert.deepEqual(await listDpaTools(redirectManifest), []);
  assert.equal(requestCount, beforeRedirect + 1);

  const missingManifestTools = await listDpaTools(path.join(tmp, "missing.json"));
  assert.deepEqual(missingManifestTools, []);

  const noReadTokenPath = path.join(tmp, "no-read-token.json");
  await writeFile(noReadTokenPath, JSON.stringify({ host: "127.0.0.1", port: address.port }));
  const beforeInvalidManifest = requestCount;
  assert.deepEqual(await listDpaTools(noReadTokenPath), []);
  assert.equal((await callDpaTool("dpa.project.current", {}, noReadTokenPath)).isError, true);
  assert.equal(requestCount, beforeInvalidManifest);

  const offLoopbackPath = path.join(tmp, "off-loopback.json");
  await writeFile(offLoopbackPath, JSON.stringify({
    host: "127.0.0.1", port: address.port, read_token: readToken,
    endpoints: { tools: "http://example.invalid/tools" }
  }));
  assert.deepEqual(await listDpaTools(offLoopbackPath), []);
  assert.equal(requestCount, beforeInvalidManifest);
} finally {
  server.close();
  await rm(tmp, { recursive: true, force: true });
}
