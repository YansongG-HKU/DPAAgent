#pragma once
#include <string>
#include <vector>
namespace Model {
    // An image produced by a tool (e.g. a captured plot). Bytes travel to the model as an
    // LlmPart(Image); toJson() emits only lightweight metadata (media_type/caption).
    struct ImageArtifact {
        std::string base64;                 // raw base64, no "data:" prefix
        std::string mediaType = "image/png";
        std::string caption;                // optional
    };
    struct ToolResult {
        bool ok = true;
        std::string summary;
        std::string dataJson = "{}";
        std::string error;
        std::vector<std::string> artifactRefs;
        // Stable references into the measurement/evidence ledger. These are
        // opaque IDs only; raw samples and paths never travel in the result.
        std::vector<std::string> evidenceRefs;
        std::vector<ImageArtifact> images;   // image channel (bytes -> LlmPart(Image))
        std::string toJson() const;   // implemented in ToolResult.cpp
    };
}
