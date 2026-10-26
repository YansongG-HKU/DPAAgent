#Requires -Version 5.1
[CmdletBinding()]
param(
    [Alias("DpaWorkspace")]
    [string]$AgentWorkspace,
    [string]$DpaMcpServer,
    [switch]$SkipNpmInstall
)
$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $AgentWorkspace) { $AgentWorkspace = Join-Path $RepoRoot ".dpa-workspace" }
if (-not $DpaMcpServer) { $DpaMcpServer = Join-Path $RepoRoot "packages\dpa-mcp-server\dist\index.js" }
$node = Get-Command node -ErrorAction Stop
$version = (& $node.Source --version).Trim()
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^v24\.') { throw "DPA MCP requires Node 24; got $version" }
Push-Location $RepoRoot
try {
    if (-not $SkipNpmInstall) {
        & npm.cmd ci --ignore-scripts --prefix packages/dpa-mcp-server
        if ($LASTEXITCODE -ne 0) { throw "MCP dependency installation failed." }
        & npm.cmd run self-check --prefix packages/dpa-mcp-server
        if ($LASTEXITCODE -ne 0) { throw "MCP build/self-check failed." }
    }
    if (-not (Test-Path -LiteralPath $DpaMcpServer -PathType Leaf)) {
        throw "Build the MCP client first: $DpaMcpServer"
    }
    New-Item -ItemType Directory -Force -Path $AgentWorkspace | Out-Null
    $configPath = Join-Path $AgentWorkspace ".mcp.json"
    $instructionsPath = Join-Path $AgentWorkspace "AGENTS.md"
    if (Test-Path -LiteralPath $configPath) {
        Write-Host "Existing MCP config preserved: $configPath"
    } else {
        $config = @{ mcpServers = @{ dpa = @{
            command = $node.Source
            args = @([IO.Path]::GetFullPath($DpaMcpServer))
            directTools = $true
            requestTimeoutMs = 90000
            env = @{ DPA_MCP_TIMEOUT_MS = "90000" }
        } } }
        [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding($false)))
    }
    if (-not (Test-Path -LiteralPath $instructionsPath)) {
        Copy-Item -LiteralPath (Join-Path $RepoRoot "AGENTS.md") -Destination $instructionsPath
    }
    Write-Host "Open Agent workspace: $AgentWorkspace"
    Write-Host "Start DPA explicitly; this client discovers only its actual read-only tools."
    Write-Host "Install/configure pi and pi-mcp-adapter separately if not already available."
    Write-Host "Desktop dependencies/build are separate: npm install; npm run dev."
} finally { Pop-Location }
