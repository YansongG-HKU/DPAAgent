#pragma once
#include <string>
namespace Model {
    // Enhanced Markdown -> Qt rich-text (HTML subset) for chat bubbles. Pure, unit-tested.
    // Supports: headings, bullet/numbered lists, **bold**, `code`, ```code blocks```,
    // [links](url), > blockquotes, --- horizontal rules, | tables |.
    std::string toRichText(const std::string& md);
}
