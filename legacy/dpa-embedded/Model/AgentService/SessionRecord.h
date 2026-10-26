#pragma once
#include <string>
#include <vector>
namespace Model {
    struct SessionTurn { std::string role; std::string content; }; // role: user|assistant|tool|operator_feedback
    struct SessionRecord {
        std::string id;
        std::string title;
        // Optional recoverable state for the grounded diagnostic workflow.
        std::string diagnosticRunJson;
        // Persist one approved write intent before dispatch. Restart recovery
        // records an unknown outcome and never replays the write.
        std::string pendingWriteJson;
        // Local monitor events are separate from prompt history, so telemetry is
        // not silently replayed to the model when a session is resumed.
        std::vector<std::string> companionEvents;
        std::vector<SessionTurn> turns;
    };
    // Serialize a session to compact, backward-compatible JSON.
    std::string buildSessionJson(const SessionRecord& s);
    // Parse it back (best-effort; used on load).
    SessionRecord parseSessionJson(const std::string& json);
}
