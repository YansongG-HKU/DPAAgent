#pragma once
#include <string>
#include <vector>
#include "ToolResult.h"
#include "Diagnostics.h"   // DiagnosticFinding (for the grounded diagnostic report section)
namespace Model {
    inline constexpr const char* kReportGeneratorVersion = "dpa-markdown-report/1.0.0";

    struct ReportSection {
        std::string title;
        std::string body;
        // Only an assessed diagnostic section may promote diagnostic provenance to the
        // report-level provenance block. Empty/unassessed sections are not proof that the
        // diagnostic engine completed successfully.
        bool diagnosticAssessed = false;
        std::string diagnosticAlgorithmVersion;
        std::string diagnosticRuleVersion;
        std::string diagnosticStandardProfile;
    };
    struct ReportProvenance {
        std::string dataId;
        int channel = -1;
        std::string analysisName;
        std::string analysisParameters;
        std::string reportGeneratorVersion = kReportGeneratorVersion;
        std::string algorithmVersion;
        std::string ruleVersion;
        std::string standardProfile;
        // Catalog metadata only; the report never embeds a copyrighted standard.
        std::string standardAuthority;
        std::string standardTitle;
        std::string standardStatus;
        std::string standardSourceUrl;
        std::string standardDisposition;
        std::string standardPlanSha256;
        std::string standardPlanDisposition;
    };
    struct ReportData {
        bool hasTest = false;
        std::string title;          // report title, e.g. test name
        ReportProvenance provenance;
        std::vector<ReportSection> sections;
    };
    // Pure core (unit-tested): assemble a Markdown report from real data.
    ToolResult buildReportResult(const ReportData& d);
    // Pure core (unit-tested): render grounded diagnostic findings (severity/evidence/root-cause/
    // recommendation) as a report section; honest note when there are no findings.
    ReportSection diagnosticFindingsSection(const std::vector<DiagnosticFinding>& findings);
    // Adapter (framework-coupled, plugin-only, NOT unit-tested): gather real data.
    ReportData gatherReportData(class M* dpaTest);
    // Adapter helper: canonical compact-JSON snapshot of properties that actually exist on
    // the current process. Empty when no process/parameters are available.
    std::string snapshotAnalysisParameters(class M* process);
}
