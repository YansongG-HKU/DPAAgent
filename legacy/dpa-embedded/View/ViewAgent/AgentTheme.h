#ifndef AGENTTHEME_H
#define AGENTTHEME_H

#include <QString>
#include <QSettings>

// =============================================================================
// AgentTheme — Design System for DPA-2026 Industrial DAQ Copilot
// =============================================================================
//
// Structure:
//   1. Surface tokens      – backgrounds and elevated surfaces
//   2. Text tokens         – foreground text hierarchy
//   3. Border tokens       – hairline and focus borders
//   4. Accent tokens       – teal accent family
//   5. Semantic tokens     – success / warning / error
//   6. Severity tokens     – info / concern / critical
//   7. Workflow tokens     – step state colours
//   8. Companion tokens    – companion health states
//   9. Auxiliary tokens    – extra palette entries used inline
//  10. Typography tokens   – font families, sizes, line-heights
//  11. Spacing tokens      – layout gap scale
//  12. Radius tokens       – border-radius scale
//  13. Shadow tokens       – QSS box-shadow strings
//  14. Style factories     – ready-made QSS for common widgets
//  15. Backward-compat aliases – old token names → new structured names
//  16. Dark-mode scaffolding   – dark palette + isDark() accessor
//
// All tokens are `constexpr const char*` so they can be used in QSS
// concatenation at compile time with zero runtime cost.
// =============================================================================

namespace AgentTheme {

// =============================================================================
// 1. Surface Tokens
// =============================================================================
constexpr const char* SURFACE_BG       = "#eef1f2";   // main window / scroll area background
constexpr const char* SURFACE_CARD     = "#ffffff";   // card / dialog body (white)
constexpr const char* SURFACE_ELEVATED = "#f7f9f9";   // header bar, campaign toggle, input bar
constexpr const char* SURFACE_INPUT    = "#ffffff";   // text-edit and input-field background
constexpr const char* SURFACE_HOVER    = "#f3f6f6";   // hover-state background (was AIBG)

// =============================================================================
// 2. Text Tokens
// =============================================================================
constexpr const char* TEXT_PRIMARY    = "#1a2730";   // headings, body text, primary labels
constexpr const char* TEXT_SECONDARY  = "#5c6b76";   // metadata, hints, muted labels
constexpr const char* TEXT_TERTIARY   = "#93a1ab";   // timestamps, assessment, placeholder
constexpr const char* TEXT_ON_ACCENT  = "#ffffff";   // text drawn on accent-coloured surfaces
constexpr const char* TEXT_DISABLED   = "#93a1ab";   // disabled state (same as tertiary)

// =============================================================================
// 3. Border Tokens
// =============================================================================
constexpr const char* BORDER_LIGHT   = "#e3e7ea";   // table widget borders
constexpr const char* BORDER_DEFAULT = "#dde4e6";   // general hairline borders (was LINE)
constexpr const char* BORDER_STRONG  = "#93a1ab";   // stronger / divider borders
constexpr const char* BORDER_FOCUS   = "#0e9e92";   // focus ring (matches accent)

// =============================================================================
// 4. Accent Tokens (teal)
// =============================================================================
constexpr const char* ACCENT_PRIMARY = "#0e9e92";   // buttons, highlights, active states
constexpr const char* ACCENT_HOVER   = "#0c8a7f";   // hover-darken for accent buttons
constexpr const char* ACCENT_PRESSED = "#0a766c";   // pressed / active-darken
constexpr const char* ACCENT_SUBTLE  = "#e8f6f4";   // light teal tint (companion toggle)

// =============================================================================
// 5. Semantic Tokens
// =============================================================================
constexpr const char* OK_GREEN   = "#2f9e54";   // success / healthy / done
constexpr const char* OK_SUBTLE  = "#d8efec";   // light green background (evidence block)
constexpr const char* WARN_AMBER = "#c98a1b";   // degraded / needs attention
constexpr const char* WARN_SUBTLE= "#f8efd9";   // light amber background (confirm card)
constexpr const char* BAD_RED    = "#c0392b";   // failure / error
constexpr const char* BAD_SUBTLE = "#fbe7e4";   // light red background (alarm card)

// =============================================================================
// 6. Severity Tokens (diagnostic / feedback card severity)
// =============================================================================
constexpr const char* SEV_INFO     = "#3b82f6";   // informational / blue severity
constexpr const char* SEV_CONCERN  = "#c07f21";   // concern / amber severity
constexpr const char* SEV_CRITICAL = "#cf4a3f";   // critical / alarm severity

// =============================================================================
// 7. Workflow Tokens (tool-step / campaign state indicators)
// =============================================================================
constexpr const char* STEP_RUNNING = "#2f77c0";   // running tool step (was STEP_BLUE)
constexpr const char* STEP_DONE    = "#2f9e54";   // completed step
constexpr const char* STEP_FAILED  = "#c0392b";   // failed step
constexpr const char* STEP_PENDING = "#93a1ab";   // pending / not-yet-started step

// =============================================================================
// 8. Companion Tokens
// =============================================================================
constexpr const char* COMPANION_HEALTHY  = "#2f9e54";   // companion fully operational
constexpr const char* COMPANION_DEGRADED = "#c98a1b";   // companion degraded / warnings
constexpr const char* COMPANION_FAILED   = "#c0392b";   // companion failed / blocked

// =============================================================================
// 9. Auxiliary Palette (used inline in existing widgets)
// =============================================================================
constexpr const char* BLUE_PRIMARY = "#1f6fb2";   // refinement-authorize / active campaign
constexpr const char* BLUE_SUBTLE  = "#e9f3f8";   // refinement card background
constexpr const char* BLUE_DARK    = "#1b5e86";   // refinement card title text
constexpr const char* AMBER_DARK   = "#8a5a10";   // confirm card title text
constexpr const char* RED_STOP     = "#c4453c";   // stop / abort button text
constexpr const char* NEUTRAL_DEFAULT = "#6b7480"; // neutral state (campaign idle)
constexpr const char* TEXT_DARK    = "#333f4b";   // CampaignPanel body text
constexpr const char* ACCENT_TEXT  = "#0a6f66";   // evidence block text (dark teal)
constexpr const char* NEUTRAL_CHIP  = "#edf0f1";  // neutral chip background (unassessed badge, metadata chips)

// =============================================================================
// 10. Typography Tokens
// =============================================================================
constexpr const char* FONT_FAMILY    = "Microsoft YaHei, Segoe UI, sans-serif";
constexpr const char* FONT_FAMILY_CN = "Microsoft YaHei";
constexpr const char* FONT_FAMILY_EN = "Segoe UI";
constexpr const char* FONT_MONO      = "Cascadia Code, Consolas, monospace";
constexpr const char* FONT_MONO_PRIMARY = "Consolas";

constexpr int FONT_SIZE_XS  = 10;   // tiny labels, badges
constexpr int FONT_SIZE_SM  = 11;   // metadata, small text
constexpr int FONT_SIZE_MD  = 12;   // body text, chip labels
constexpr int FONT_SIZE_LG  = 14;   // titles, header
constexpr int FONT_SIZE_XL  = 16;   // section headings
constexpr int FONT_SIZE_XXL = 20;   // display headings

constexpr const char* LINE_HEIGHT_TIGHT = "1.2";
constexpr const char* LINE_HEIGHT_NORMAL = "1.5";

// =============================================================================
// 11. Spacing Tokens
// =============================================================================
constexpr int SP_XS  = 2;    // micro gap (between dot and label)
constexpr int SP_SM  = 4;    // tight internal gap
constexpr int SP_MD  = 8;    // standard internal padding
constexpr int SP_LG  = 12;   // card padding, stream margins
constexpr int SP_XL  = 16;   // dialog margins
constexpr int SP_XXL = 24;   // section separation

// =============================================================================
// 12. Border Radius Tokens
// =============================================================================
constexpr int RADIUS_SM   = 4;    // small elements (chip inner)
constexpr int RADIUS_MD   = 6;    // tool buttons, details
constexpr int RADIUS_LG   = 8;    // cards, confirm buttons
constexpr int RADIUS_XL   = 10;   // card frames, input fields
constexpr int RADIUS_PILL = 999;  // pill buttons, companion toggle

// =============================================================================
// 13. Shadow Tokens (QSS box-shadow — note: Qt 5 does not support box-shadow
//     natively in stylesheets; these are provided for future use or custom
//     painting.  Qt 6 with QSS extensions may support them.)
// =============================================================================
constexpr const char* SHADOW_SM = "0 1px 2px rgba(0,0,0,0.06)";
constexpr const char* SHADOW_MD = "0 2px 6px rgba(0,0,0,0.08)";
constexpr const char* SHADOW_LG = "0 4px 12px rgba(0,0,0,0.12)";

// =============================================================================
// 13b. Theme-aware accessor forward declarations
// =============================================================================
// The style factories below resolve tokens through currentXxx() accessors that
// choose light/dark values at runtime. The accessor bodies sit near the bottom
// of this header (with isDark()); declare them here so the factories above the
// definitions compile.
inline const char* currentSurfaceBg();
inline const char* currentSurfaceCard();
inline const char* currentSurfaceElevated();
inline const char* currentSurfaceInput();
inline const char* currentSurfaceHover();
inline const char* currentTextPrimary();
inline const char* currentTextSecondary();
inline const char* currentTextTertiary();
inline const char* currentTextOnAccent();
inline const char* currentBorderLight();
inline const char* currentBorderDefault();
inline const char* currentBorderStrong();
inline const char* currentBorderFocus();
inline const char* currentAccentPrimary();
inline const char* currentAccentHover();
inline const char* currentAccentPressed();
inline const char* currentAccentSubtle();
inline const char* currentOkGreen();
inline const char* currentOkSubtle();
inline const char* currentWarnAmber();
inline const char* currentWarnSubtle();
inline const char* currentBadRed();
inline const char* currentBadSubtle();
inline const char* currentStepRunning();
inline const char* currentStepDone();
inline const char* currentStepFailed();
inline const char* currentStepPending();
inline const char* currentCompanionHealthy();
inline const char* currentCompanionDegraded();
inline const char* currentCompanionFailed();
inline const char* currentBluePrimary();
inline const char* currentBlueSubtle();
inline const char* currentBlueDark();
inline const char* currentAmberDark();
inline const char* currentRedStop();
inline const char* currentNeutralDefault();
inline const char* currentTextDark();
inline const char* currentAccentText();
inline const char* currentNeutralChip();
inline const char* currentSevInfo();
inline const char* currentSevConcern();
inline const char* currentSevCritical();

// =============================================================================
// 14. Style Factory Functions
// =============================================================================

// Pill-shaped quick-action chip (hover border follows accent).
inline QString chipButtonStyle() {
    return QStringLiteral(
        "QToolButton{"
            "border:1px solid %1;"
            "border-radius:%2px;"
            "padding:4px 11px;"
            "background:%3;"
            "color:%4;"
            "font-size:%5px;"
        "}"
        "QToolButton:hover{border-color:%4;}"
    )
    .arg(QLatin1String(currentBorderDefault()))
    .arg(RADIUS_PILL)
    .arg(QLatin1String(currentSurfaceBg()))
    .arg(QLatin1String(currentAccentPrimary()))
    .arg(FONT_SIZE_MD);
}

// Rounded white frame for collapsible cards (matches WorkflowCard's #wfCard rule).
inline QString cardFrameStyle() {
    return QStringLiteral(
        "#wfCard{"
            "background:%1;"
            "border:1px solid %2;"
            "border-radius:%3px;"
        "}"
    )
    .arg(QLatin1String(currentSurfaceCard()))
    .arg(QLatin1String(currentBorderDefault()))
    .arg(RADIUS_XL);
}

// Text input field (QTextEdit / QLineEdit).
inline QString inputFieldStyle() {
    return QStringLiteral(
        "QTextEdit, QLineEdit{"
            "border:1px solid %1;"
            "border-radius:%2px;"
            "padding:7px 9px;"
            "background:%3;"
            "color:%4;"
            "font-size:%5px;"
            "selection-background-color:%6;"
        "}"
        "QTextEdit:focus, QLineEdit:focus{"
            "border-color:%7;"
        "}"
    )
    .arg(QLatin1String(currentBorderDefault()))
    .arg(RADIUS_XL)
    .arg(QLatin1String(currentSurfaceInput()))
    .arg(QLatin1String(currentTextPrimary()))
    .arg(FONT_SIZE_LG)
    .arg(QLatin1String(currentAccentSubtle()))
    .arg(QLatin1String(currentBorderFocus()));
}

// Header tool buttons (✎ new session, ◷ history, ⓘ runtime).
inline QString toolButtonStyle() {
    return QStringLiteral(
        "QToolButton{"
            "border:1px solid %1;"
            "border-radius:%2px;"
            "padding:2px 6px;"
            "color:%3;"
            "background:%4;"
        "}"
        "QToolButton:hover{"
            "background:%5;"
            "border-color:%6;"
        "}"
    )
    .arg(QLatin1String(currentBorderDefault()))
    .arg(RADIUS_MD)
    .arg(QLatin1String(currentTextSecondary()))
    .arg(QLatin1String(currentSurfaceCard()))
    .arg(QLatin1String(currentSurfaceHover()))
    .arg(QLatin1String(currentBorderStrong()));
}

// Progress bar (indeterminate or determinate).
inline QString progressBarStyle() {
    return QStringLiteral(
        "QProgressBar{"
            "background:%1;"
            "border:1px solid %2;"
            "border-radius:%3px;"
            "height:8px;"
            "text-align:center;"
            "color:transparent;"
        "}"
        "QProgressBar::chunk{"
            "background:%4;"
            "border-radius:%5px;"
        "}"
    )
    .arg(QLatin1String(currentSurfaceHover()))
    .arg(QLatin1String(currentBorderDefault()))
    .arg(RADIUS_SM)
    .arg(QLatin1String(currentAccentPrimary()))
    .arg(RADIUS_SM);
}

// Severity badge (small inline pill for diagnostic findings).
//   severity: "alarm" | "warn" | "info" | "ok"
inline QString severityBadgeStyle(const QString& severity) {
    const char* bg   = currentSurfaceHover();
    const char* fg   = currentTextSecondary();
    if (severity == QLatin1String("alarm") || severity == QLatin1String("critical")) {
        bg = currentBadSubtle();  fg = currentSevCritical();
    } else if (severity == QLatin1String("warn") || severity == QLatin1String("concern")) {
        bg = currentWarnSubtle(); fg = currentSevConcern();
    } else if (severity == QLatin1String("info")) {
        bg = currentBlueSubtle(); fg = currentSevInfo();
    } else if (severity == QLatin1String("ok")) {
        bg = currentOkSubtle();   fg = currentOkGreen();
    }
    return QStringLiteral(
        "background:%1;color:%2;border-radius:%3px;padding:1px 7px;"
        "font-family:%4;font-size:%5px;font-weight:bold;"
    )
    .arg(QLatin1String(bg))
    .arg(QLatin1String(fg))
    .arg(RADIUS_SM)
    .arg(QLatin1String(FONT_MONO_PRIMARY))
    .arg(FONT_SIZE_XS);
}

// Workflow step indicator dot colour.
//   state: "running" | "done" | "failed" | "pending"
inline QString workflowStepStyle(const QString& state) {
    const char* color = currentStepPending();
    if (state == QLatin1String("running"))  color = currentStepRunning();
    else if (state == QLatin1String("done"))    color = currentStepDone();
    else if (state == QLatin1String("failed"))  color = currentStepFailed();
    return QStringLiteral("color:%1;font-weight:bold;").arg(QLatin1String(color));
}

// Table widget (QTableWidget / QTableView).
inline QString tableStyle() {
    return QStringLiteral(
        "QTableWidget, QTableView{"
            "font-size:%1px;"
            "border:1px solid %2;"
            "background:%3;"
            "color:%4;"
            "gridline-color:%5;"
            "alternate-background-color:%6;"
            "selection-background-color:%7;"
            "selection-color:%8;"
            "outline:none;"
        "}"
        "QTableWidget::item, QTableView::item{"
            "padding:3px 5px;"
        "}"
        "QHeaderView::section{"
            "background:%9;"
            "color:%10;"
            "border:1px solid %2;"
            "padding:3px 6px;"
            "font-weight:bold;"
            "font-size:%1px;"
        "}"
    )
    .arg(FONT_SIZE_SM)
    .arg(QLatin1String(currentBorderLight()))
    .arg(QLatin1String(currentSurfaceCard()))
    .arg(QLatin1String(currentTextDark()))
    .arg(QLatin1String(currentBorderLight()))
    .arg(QLatin1String(currentSurfaceHover()))
    .arg(QLatin1String(currentAccentSubtle()))
    .arg(QLatin1String(currentTextPrimary()))
    .arg(QLatin1String(currentSurfaceElevated()))
    .arg(QLatin1String(currentTextSecondary()));
}

// Scroll area (transparent background, thin styled scrollbar).
inline QString scrollAreaStyle() {
    return QStringLiteral(
        "QScrollArea{"
            "background:%1;"
            "border:none;"
        "}"
        "QScrollBar:vertical{"
            "background:transparent;"
            "width:8px;"
            "margin:0;"
        "}"
        "QScrollBar::handle:vertical{"
            "background:%2;"
            "border-radius:3px;"
            "min-height:20px;"
        "}"
        "QScrollBar::handle:vertical:hover{"
            "background:%3;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical{"
            "height:0px;"
            "background:transparent;"
        "}"
        "QScrollBar:horizontal{"
            "background:transparent;"
            "height:8px;"
            "margin:0;"
        "}"
        "QScrollBar::handle:horizontal{"
            "background:%2;"
            "border-radius:3px;"
            "min-width:20px;"
        "}"
        "QScrollBar::handle:horizontal:hover{"
            "background:%3;"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal{"
            "width:0px;"
            "background:transparent;"
        "}"
    )
    .arg(QLatin1String(currentSurfaceBg()))
    .arg(QLatin1String(currentBorderDefault()))
    .arg(QLatin1String(currentBorderStrong()));
}

// Generic primary action button (e.g. send, confirm, authorize).
inline QString primaryButtonStyle() {
    return QStringLiteral(
        "QPushButton{"
            "background:%1;"
            "color:%2;"
            "border:none;"
            "border-radius:%3px;"
            "padding:6px 14px;"
            "font-weight:bold;"
        "}"
        "QPushButton:hover{background:%4;}"
        "QPushButton:pressed{background:%5;}"
    )
    .arg(QLatin1String(currentAccentPrimary()))
    .arg(QLatin1String(currentTextOnAccent()))
    .arg(RADIUS_MD)
    .arg(QLatin1String(currentAccentHover()))
    .arg(QLatin1String(currentAccentPressed()));
}

// Secondary / ghost button (outline style).
inline QString secondaryButtonStyle() {
    return QStringLiteral(
        "QPushButton{"
            "background:%1;"
            "color:%2;"
            "border:1px solid %3;"
            "border-radius:%4px;"
            "padding:6px 14px;"
        "}"
        "QPushButton:hover{background:%5;border-color:%6;}"
    )
    .arg(QLatin1String(currentSurfaceCard()))
    .arg(QLatin1String(currentTextSecondary()))
    .arg(QLatin1String(currentBorderDefault()))
    .arg(RADIUS_MD)
    .arg(QLatin1String(currentSurfaceHover()))
    .arg(QLatin1String(currentBorderStrong()));
}

// Danger button (stop / abort).
inline QString dangerButtonStyle() {
    return QStringLiteral(
        "QPushButton{"
            "background:%1;"
            "color:%2;"
            "border:1px solid %3;"
            "border-radius:%4px;"
            "padding:6px 14px;"
        "}"
        "QPushButton:hover{background:%5;color:%6;}"
    )
    .arg(QLatin1String(currentSurfaceCard()))
    .arg(QLatin1String(currentRedStop()))
    .arg(QLatin1String(currentBorderDefault()))
    .arg(RADIUS_MD)
    .arg(currentBadSubtle())
    .arg(QLatin1String(currentBadRed()));
}

// ============================================================================
// 15. Backward-Compatibility Aliases
//
// These map the old flat token names onto the new structured system so that
// existing code in AgentChatDock.cpp / CampaignPanel.cpp continues to compile
// without changes.
// ============================================================================
constexpr const char* ACCENT  = ACCENT_PRIMARY;   // was #0e9e92
constexpr const char* INK     = TEXT_PRIMARY;      // was #1a2730
constexpr const char* MUTE    = TEXT_SECONDARY;    // was #5c6b76
constexpr const char* LINE    = BORDER_DEFAULT;    // was #dde4e6
constexpr const char* AIBG    = SURFACE_HOVER;     // was #f3f6f6 (AI bubble bg)
constexpr const char* STEP_BLUE = STEP_RUNNING;    // was #2f77c0

// =============================================================================
// 16. Dark-Mode Scaffolding
// =============================================================================
namespace dark {
constexpr const char* SURFACE_BG       = "#1a1d21";
constexpr const char* SURFACE_CARD     = "#22262b";
constexpr const char* SURFACE_ELEVATED = "#2a2e34";
constexpr const char* SURFACE_INPUT    = "#22262b";
constexpr const char* SURFACE_HOVER    = "#2e3339";

constexpr const char* TEXT_PRIMARY    = "#e4e7ea";
constexpr const char* TEXT_SECONDARY  = "#93a1ab";
constexpr const char* TEXT_TERTIARY   = "#6b7480";
constexpr const char* TEXT_ON_ACCENT  = "#ffffff";
constexpr const char* TEXT_DISABLED   = "#505760";

constexpr const char* BORDER_LIGHT   = "#333a42";
constexpr const char* BORDER_DEFAULT = "#3b424a";
constexpr const char* BORDER_STRONG  = "#6b7480";
constexpr const char* BORDER_FOCUS   = "#0e9e92";

constexpr const char* ACCENT_PRIMARY = "#14c4b7";
constexpr const char* ACCENT_HOVER   = "#18d4c7";
constexpr const char* ACCENT_PRESSED = "#10a89d";
constexpr const char* ACCENT_SUBTLE  = "#1a3330";

constexpr const char* OK_GREEN   = "#4ade80";
constexpr const char* OK_SUBTLE  = "#1a332a";
constexpr const char* WARN_AMBER = "#f59e0b";
constexpr const char* WARN_SUBTLE= "#332a1a";
constexpr const char* BAD_RED    = "#ef4444";
constexpr const char* BAD_SUBTLE = "#331a1a";

constexpr const char* SEV_INFO     = "#60a5fa";
constexpr const char* SEV_CONCERN  = "#f59e0b";
constexpr const char* SEV_CRITICAL = "#f87171";

constexpr const char* STEP_RUNNING = "#60a5fa";
constexpr const char* STEP_DONE    = "#4ade80";
constexpr const char* STEP_FAILED  = "#ef4444";
constexpr const char* STEP_PENDING = "#6b7480";

constexpr const char* COMPANION_HEALTHY  = "#4ade80";
constexpr const char* COMPANION_DEGRADED = "#f59e0b";
constexpr const char* COMPANION_FAILED   = "#ef4444";

constexpr const char* BLUE_PRIMARY = "#3b82f6";
constexpr const char* BLUE_SUBTLE  = "#1a2533";
constexpr const char* BLUE_DARK    = "#93c5fd";
constexpr const char* AMBER_DARK   = "#fbbf24";
constexpr const char* RED_STOP     = "#f87171";
constexpr const char* NEUTRAL_DEFAULT = "#93a1ab";
constexpr const char* TEXT_DARK    = "#d1d5db";
constexpr const char* ACCENT_TEXT  = "#5eead4";
constexpr const char* NEUTRAL_CHIP  = "#2e3339";
} // namespace dark

// One-shot dark-mode cache shared by isDark() / setDarkMode() / toggleDarkMode().
namespace detail {
inline bool& darkModeCacheValid() { static bool valid = false; return valid; }
inline bool& darkModeCacheValue()  { static bool value  = false; return value; }
} // namespace detail

// Returns true when the user has selected dark mode via QSettings
// (registry key HKCU\Software\DPA\DPA-2026\AgentTheme\dark).
inline bool isDark() {
    if (!detail::darkModeCacheValid()) {
        QSettings s("DPA", "DPA-2026");
        detail::darkModeCacheValue() = s.value("AgentTheme/dark", false).toBool();
        detail::darkModeCacheValid() = true;
    }
    return detail::darkModeCacheValue();
}

// Persists the dark-mode choice and invalidates the cache so the next
// construction picks up the new palette immediately.
inline void setDarkMode(bool enabled) {
    QSettings s("DPA", "DPA-2026");
    s.setValue("AgentTheme/dark", enabled);
    s.sync();
    detail::darkModeCacheValue() = enabled;
    detail::darkModeCacheValid() = true;
}

// Flips the persisted theme and persists the new value.
inline void toggleDarkMode() {
    setDarkMode(!isDark());
}

// Resets the cached dark-mode value so the next isDark() re-reads QSettings.
inline void resetDarkCache() {
    detail::darkModeCacheValid() = false;
}

// ---- currentXxx() accessors: return light or dark token based on isDark() ----

inline const char* currentSurfaceBg()       { return isDark() ? dark::SURFACE_BG       : SURFACE_BG; }
inline const char* currentSurfaceCard()     { return isDark() ? dark::SURFACE_CARD     : SURFACE_CARD; }
inline const char* currentSurfaceElevated() { return isDark() ? dark::SURFACE_ELEVATED : SURFACE_ELEVATED; }
inline const char* currentSurfaceInput()    { return isDark() ? dark::SURFACE_INPUT    : SURFACE_INPUT; }
inline const char* currentSurfaceHover()    { return isDark() ? dark::SURFACE_HOVER    : SURFACE_HOVER; }

inline const char* currentTextPrimary()    { return isDark() ? dark::TEXT_PRIMARY    : TEXT_PRIMARY; }
inline const char* currentTextSecondary()  { return isDark() ? dark::TEXT_SECONDARY  : TEXT_SECONDARY; }
inline const char* currentTextTertiary()   { return isDark() ? dark::TEXT_TERTIARY   : TEXT_TERTIARY; }
inline const char* currentTextOnAccent()   { return isDark() ? dark::TEXT_ON_ACCENT  : TEXT_ON_ACCENT; }

inline const char* currentBorderLight()    { return isDark() ? dark::BORDER_LIGHT    : BORDER_LIGHT; }
inline const char* currentBorderDefault()  { return isDark() ? dark::BORDER_DEFAULT  : BORDER_DEFAULT; }
inline const char* currentBorderStrong()   { return isDark() ? dark::BORDER_STRONG   : BORDER_STRONG; }
inline const char* currentBorderFocus()    { return isDark() ? dark::BORDER_FOCUS    : BORDER_FOCUS; }

inline const char* currentAccentPrimary()  { return isDark() ? dark::ACCENT_PRIMARY  : ACCENT_PRIMARY; }
inline const char* currentAccentHover()    { return isDark() ? dark::ACCENT_HOVER    : ACCENT_HOVER; }
inline const char* currentAccentPressed()  { return isDark() ? dark::ACCENT_PRESSED  : ACCENT_PRESSED; }
inline const char* currentAccentSubtle()   { return isDark() ? dark::ACCENT_SUBTLE   : ACCENT_SUBTLE; }

inline const char* currentOkGreen()    { return isDark() ? dark::OK_GREEN    : OK_GREEN; }
inline const char* currentOkSubtle()   { return isDark() ? dark::OK_SUBTLE   : OK_SUBTLE; }
inline const char* currentWarnAmber()  { return isDark() ? dark::WARN_AMBER  : WARN_AMBER; }
inline const char* currentWarnSubtle() { return isDark() ? dark::WARN_SUBTLE : WARN_SUBTLE; }
inline const char* currentBadRed()     { return isDark() ? dark::BAD_RED     : BAD_RED; }
inline const char* currentBadSubtle()  { return isDark() ? dark::BAD_SUBTLE  : BAD_SUBTLE; }

inline const char* currentStepRunning()  { return isDark() ? dark::STEP_RUNNING  : STEP_RUNNING; }
inline const char* currentStepDone()     { return isDark() ? dark::STEP_DONE     : STEP_DONE; }
inline const char* currentStepFailed()   { return isDark() ? dark::STEP_FAILED   : STEP_FAILED; }
inline const char* currentStepPending()  { return isDark() ? dark::STEP_PENDING  : STEP_PENDING; }

inline const char* currentCompanionHealthy()  { return isDark() ? dark::COMPANION_HEALTHY  : COMPANION_HEALTHY; }
inline const char* currentCompanionDegraded() { return isDark() ? dark::COMPANION_DEGRADED : COMPANION_DEGRADED; }
inline const char* currentCompanionFailed()   { return isDark() ? dark::COMPANION_FAILED   : COMPANION_FAILED; }

inline const char* currentBluePrimary()   { return isDark() ? dark::BLUE_PRIMARY   : BLUE_PRIMARY; }
inline const char* currentBlueSubtle()    { return isDark() ? dark::BLUE_SUBTLE    : BLUE_SUBTLE; }
inline const char* currentBlueDark()      { return isDark() ? dark::BLUE_DARK      : BLUE_DARK; }
inline const char* currentAmberDark()     { return isDark() ? dark::AMBER_DARK     : AMBER_DARK; }
inline const char* currentRedStop()       { return isDark() ? dark::RED_STOP       : RED_STOP; }
inline const char* currentNeutralDefault(){ return isDark() ? dark::NEUTRAL_DEFAULT : NEUTRAL_DEFAULT; }
inline const char* currentTextDark()      { return isDark() ? dark::TEXT_DARK      : TEXT_DARK; }
inline const char* currentAccentText()    { return isDark() ? dark::ACCENT_TEXT    : ACCENT_TEXT; }
inline const char* currentNeutralChip()   { return isDark() ? dark::NEUTRAL_CHIP   : NEUTRAL_CHIP; }

inline const char* currentSevInfo()     { return isDark() ? dark::SEV_INFO     : SEV_INFO; }
inline const char* currentSevConcern()  { return isDark() ? dark::SEV_CONCERN  : SEV_CONCERN; }
inline const char* currentSevCritical() { return isDark() ? dark::SEV_CRITICAL : SEV_CRITICAL; }

} // namespace AgentTheme

#endif // AGENTTHEME_H
