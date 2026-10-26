#pragma once
#include <functional>
#include <map>
#include <string>
#include "ToolResult.h"
namespace Model {
    struct AgentTool {
        std::string name;
        std::string description;
        std::string inputSchemaJson;   // a JSON Schema string
        std::function<ToolResult(const std::string& argsJson)> handler;
        std::string mcpName;
        std::string title;
        std::string outputSchemaJson = R"({"type":"object"})";
        std::string category = "analysis";
        // Must be set explicitly to "read" or "write" before registration.
        std::string dangerLevel = "unclassified";
    };
    class ToolRegistry {
    public:
        // Returns false and does not register the tool when either schema is not a valid
        // JSON object. This keeps malformed schemas fail-closed instead of silently becoming {}.
        bool add(const AgentTool& t);
        bool has(const std::string& name) const;
        bool hasMcp(const std::string& mcpName) const;
        std::string approvalCatalogSha256() const;
        std::string listJson() const;  // [{name,description,input_schema}...]
        std::string listMcpJson() const;
        ToolResult call(const std::string& name, const std::string& argsJson) const;
        ToolResult callMcp(const std::string& mcpName, const std::string& argsJson) const;
        // Gate source of truth. Anything other than an explicitly registered read tool
        // is treated as requiring the write/deny gate.
        bool isWrite(const std::string& name) const;         // by internal name
        bool isWriteMcp(const std::string& mcpName) const;    // by MCP name (dpa.*)
    private:
        std::map<std::string, AgentTool> _tools;
    };
}
