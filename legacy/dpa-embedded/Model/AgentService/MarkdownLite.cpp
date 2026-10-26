#include "MarkdownLite.h"
#include <vector>
#include <algorithm>
using namespace Model;

static std::string escape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

// Alternating replace of a paired marker with open/close tags (on already-escaped text).
static std::string pairWrap(std::string s, const std::string& mark, const std::string& open, const std::string& close) {
    std::string o; size_t i = 0; bool inside = false;
    while (i < s.size()) {
        if (s.compare(i, mark.size(), mark) == 0) {
            o += inside ? close : open;
            inside = !inside;
            i += mark.size();
        } else { o += s[i++]; }
    }
    if (inside) o += close;
    return o;
}

// Split string by delimiter
static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

// Trim whitespace
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    size_t end = s.find_last_not_of(" \t");
    return start == std::string::npos ? "" : s.substr(start, end - start + 1);
}

// Inline formatting: bold, inline code, links
static std::string inlineFmt(const std::string& raw) {
    std::string s = escape(raw);
    s = pairWrap(s, "**", "<b>", "</b>");
    s = pairWrap(s, "__", "<b>", "</b>");
    s = pairWrap(s, "*", "<i>", "</i>");
    s = pairWrap(s, "_", "<i>", "</i>");
    s = pairWrap(s, "`", "<code style='background:rgba(128,128,128,0.15);padding:1px 4px;border-radius:3px;font-family:Consolas,monospace;font-size:11px;'>", "</code>");
    // Links: [text](url) - after escaping, the URL may contain &amp; so we handle that
    {
        std::string result;
        size_t i = 0;
        while (i < s.size()) {
            size_t lb = s.find('[', i);
            if (lb == std::string::npos) { result += s.substr(i); break; }
            size_t rb = s.find(']', lb);
            if (rb == std::string::npos) { result += s.substr(i); break; }
            size_t lp = s.find('(', rb);
            if (lp == std::string::npos || lp - rb > 1) { result += s.substr(i, lb - i + 1); i = lb + 1; continue; }
            size_t rp = s.find(')', lp);
            if (rp == std::string::npos) { result += s.substr(i, lb - i + 1); i = lb + 1; continue; }
            // Valid link: [text](url)
            std::string linkText = s.substr(lb + 1, rb - lb - 1);
            std::string url = s.substr(lp + 1, rp - lp - 1);
            result += s.substr(i, lb - i);
            result += "<a href='" + url + "' style='color:#0e9e92;text-decoration:none;'>" + linkText + "</a>";
            i = rp + 1;
        }
        s = result;
    }
    return s;
}

// Check if a line is a table separator (e.g., |---|---|)
static bool isTableSeparator(const std::string& line) {
    std::string t = trim(line);
    if (t.empty() || t[0] != '|') return false;
    for (size_t i = 1; i < t.size() - 1; ++i) {
        if (t[i] != '-' && t[i] != ':' && t[i] != ' ' && t[i] != '|')
            return false;
    }
    return true;
}

// Split a table row into cells
static std::vector<std::string> splitTableRow(const std::string& line) {
    std::vector<std::string> cells;
    std::string t = trim(line);
    if (!t.empty() && t.front() == '|') t = t.substr(1);
    if (!t.empty() && t.back() == '|') t.pop_back();
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t sep = t.find('|', pos);
        std::string cell = t.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        cells.push_back(trim(cell));
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return cells;
}

// Render a table from header + separator + data rows
static std::string renderTable(const std::vector<std::string>& headerCells,
                                const std::vector<std::vector<std::string>>& dataRows) {
    std::string html = "<table style='border-collapse:collapse;margin:6px 0;font-size:12px;width:100%'>";
    html += "<tr>";
    for (const auto& cell : headerCells)
        html += "<th style='border:1px solid #dde4e6;padding:4px 8px;background:#f7f9f9;font-weight:bold;text-align:left;'>"
              + inlineFmt(cell) + "</th>";
    html += "</tr>";
    for (const auto& row : dataRows) {
        html += "<tr>";
        for (size_t i = 0; i < row.size(); ++i)
            html += "<td style='border:1px solid #dde4e6;padding:4px 8px;'>"
                  + inlineFmt(row[i]) + "</td>";
        html += "</tr>";
    }
    html += "</table>";
    return html;
}

std::string Model::toRichText(const std::string& md) {
    auto lines = splitLines(md);
    std::string out;
    bool listOpen = false;
    bool inCodeBlock = false;
    std::string codeBlockContent;
    std::string codeBlockLang;

    auto closeList = [&]() { if (listOpen) { out += "</ul>"; listOpen = false; } };

    for (size_t idx = 0; idx < lines.size(); ++idx) {
        const std::string& line = lines[idx];

        // Fenced code blocks: ```lang ... ```
        if (line.find("```") == 0) {
            closeList();
            if (!inCodeBlock) {
                inCodeBlock = true;
                codeBlockLang = trim(line.substr(3));
                codeBlockContent.clear();
            } else {
                // End of code block
                std::string langLabel;
                if (!codeBlockLang.empty())
                    langLabel = "<div style='color:#5c6b76;font-size:10px;font-family:Consolas,monospace;margin-bottom:2px;'>"
                              + escape(codeBlockLang) + "</div>";
                out += "<div style='background:#1e2430;color:#e4e7ea;border-radius:6px;padding:8px 10px;margin:4px 0;font-family:Consolas,monospace;font-size:11px;white-space:pre-wrap;overflow-x:auto;'>"
                     + langLabel
                     + "<pre style='margin:0;white-space:pre-wrap;'>" + escape(codeBlockContent) + "</pre></div>";
                inCodeBlock = false;
                codeBlockContent.clear();
                codeBlockLang.clear();
            }
            continue;
        }
        if (inCodeBlock) {
            if (!codeBlockContent.empty()) codeBlockContent += "\n";
            codeBlockContent += line;
            continue;
        }

        // Blank line
        if (line.empty()) {
            closeList();
            out += "<br>";
            continue;
        }

        // Horizontal rule
        if (trim(line) == "---" || trim(line) == "***" || trim(line) == "___") {
            closeList();
            out += "<hr style='border:none;border-top:1px solid #dde4e6;margin:6px 0;'>";
            continue;
        }

        // Headings: ## to ######
        if (line.size() > 1 && line[0] == '#') {
            closeList();
            int level = 0;
            while (level < (int)line.size() - 1 && line[level + 1] == '#') ++level;
            level += 1; // h1 for single #
            std::string heading = trim(line.substr(level + 1));
            int fontSize = std::max(12, 20 - level * 2);
            out += "<div style='font-size:" + std::to_string(fontSize) + "px;font-weight:bold;margin:6px 0 2px 0;color:#1a2730;'>"
                 + inlineFmt(heading) + "</div>";
            continue;
        }

        // Blockquote: > text
        if (line.size() > 2 && line[0] == '>' && line[1] == ' ') {
            closeList();
            out += "<div style='border-left:3px solid #0e9e92;padding:4px 10px;margin:4px 0;background:#e8f6f4;border-radius:0 4px 4px 0;color:#1a2730;'>"
                 + inlineFmt(line.substr(2)) + "</div>";
            continue;
        }

        // Table detection: check if current line + next line form a table
        if (line.find('|') != std::string::npos && idx + 1 < lines.size() && isTableSeparator(lines[idx + 1])) {
            closeList();
            auto headerCells = splitTableRow(line);
            idx++; // skip separator
            std::vector<std::vector<std::string>> dataRows;
            while (idx + 1 < lines.size() && lines[idx + 1].find('|') != std::string::npos && !lines[idx + 1].empty()) {
                dataRows.push_back(splitTableRow(lines[idx + 1]));
                idx++;
            }
            out += renderTable(headerCells, dataRows);
            continue;
        }

        // Numbered list: 1. 2. etc.
        if (line.size() > 2 && line[0] >= '0' && line[0] <= '9' && line[1] == '.' && line[2] == ' ') {
            closeList();
            std::string content = inlineFmt(line.substr(3));
            out += "<div style='margin:2px 0 2px 16px;'>" + std::string(1, line[0]) + ". " + content + "</div>";
            continue;
        }

        // Bullet list: - or *
        if ((line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) && line.size() > 2) {
            if (!listOpen) { out += "<ul style='margin:2px 0 2px 4px;-qt-list-indent:1'>"; listOpen = true; }
            out += "<li>" + inlineFmt(line.substr(2)) + "</li>";
            continue;
        }

        // Regular paragraph
        closeList();
        out += inlineFmt(line) + "<br>";
    }

    // Close any open code block
    if (inCodeBlock && !codeBlockContent.empty()) {
        std::string escaped = escape(codeBlockContent);
        out += "<div style='background:#1e2430;color:#e4e7ea;border-radius:6px;padding:8px 10px;margin:4px 0;font-family:Consolas,monospace;font-size:11px;white-space:pre-wrap;'>";
        out += "<pre style='margin:0;white-space:pre-wrap;'>";
        out += escaped;
        out += "</pre></div>";
    }
    closeList();
    return out;
}
