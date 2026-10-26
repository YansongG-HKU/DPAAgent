# DPA Agent

基于 [pi Desktop](https://github.com/justhil/pi-app) 的 DPA 外部桌面实验助手。
自 2026-10-26 起，本仓库承接智能化开发：对话、模型、规划、实验编排、诊断建议与评测。
原生设备、采集、存储、DSP、界面和执行联锁由 DPA 产品维护。

[安装与接口说明](docs/DPA-AGENT.md) · [迁移状态与成果清单](docs/migration/HANDOFF-20261026.md) ·
[治理](docs/GOVERNANCE.md) · [来源与许可](NOTICE.md) · [上游说明](README.upstream.md)

```text
DPAAgent / pi Desktop
  → packages/dpa-mcp-server（只读 MCP 客户端）
  → DPA 本机兼容 bridge
  → 原生受控工具与数据
```

## 当前接收的成果

- `packages/dpa-mcp-server/`：实际 DPA 协议的只读客户端与原测试。
- `tools/stats-sidecar/`：统计优化、序贯边界、离线 CUSUM/EWMA 与有误差界压缩及测试。
- `legacy/dpa-domain/`：无 Qt/FFTW 依赖的 Hilbert/包络计算核心。
- `legacy/dpa-embedded/`：原 C++/Qt 智能化实现、模型/会话/诊断/试验编排、界面和测试的冻结源码。
- `legacy/monitoring-wip/`：独立隔离的历史监控 WIP，保留已知问题。

203 份精选来源文件均记录固定提交、blob 和 SHA-256；完整私有历史、WIP 和原验收材料
保留在私有归档。源码已迁入不代表全部接入桌面、实机或模型验收完成；待办继续保留。

## 开始使用

需要 Node 24、可运行的 DPA、已配置的 pi 模型与 pi-mcp-adapter。

```powershell
git clone https://github.com/YansongG-HKU/DPAAgent.git
cd DPAAgent
.\scripts\setup-dpa-agent.ps1
npm install
npm run dev
```

打开 setup 输出的独立 Agent 工作区，先手动启动 DPA，再读取实际 MCP 工具目录。
迁移版 DPA 需显式设置 `DPA_ENABLE_AGENT_BRIDGE=1`。
当前工具名以 live catalog 为准（如 `dpa.project.current`），只开放只读能力。
旧文档中的 `dpa_ensure_running`、`dpa_*` 写工具和另一套 bridge 不作为已支持能力。

## 验证

```sh
npm ci --ignore-scripts --prefix packages/dpa-mcp-server
npm run self-check --prefix packages/dpa-mcp-server
python -m unittest discover -s tools/stats-sidecar -p test_monitor_analysis.py
python scripts/verify-dpa-migration.py
```

桌面壳上游文档和原有源码继续保留。迁入源码的许可状态见 NOTICE.md，不因 package.json
的 MIT 元数据自动获得重新授权。完整科学依赖、桌面、真实 DPA、模型和设备验收分别执行。
