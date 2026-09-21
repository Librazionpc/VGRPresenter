#include "modules/presentation/BlockValidator.hpp"

#include "core/config/Json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <set>

namespace bps::presentation {

namespace {

constexpr const char* kModule = "BlockValidator";

constexpr size_t kMaxIdLength = 80;
constexpr size_t kMaxKindLength = 40;
constexpr size_t kMaxBindLength = 40;
constexpr size_t kMaxTextLength = 200000;
constexpr size_t kMaxMetaLength = 100000;
constexpr double kMaxPosition = 100000.0;
constexpr double kMaxSize = 100000.0;
constexpr double kMaxStyleValue = 10000.0;

bool AllOf(std::string_view s, bool (*ok)(unsigned char)) {
    return std::all_of(s.begin(), s.end(), [&](char c) { return ok(static_cast<unsigned char>(c)); });
}

bool IsPrintable(unsigned char c) { return c >= 0x20 && c != 0x7f; }
bool IsKindChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }
bool IsBindChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; }
bool IsHexDigit(unsigned char c) { return std::isxdigit(c) != 0; }
bool IsLetter(unsigned char c) { return std::isalpha(c) != 0; }

bool InRange(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

} // namespace

bool IsColorText(std::string_view text) {
    if (text == "transparent") return true;
    if (!text.empty() && text.front() == '#') {
        const std::string_view digits = text.substr(1);
        return (digits.size() == 3 || digits.size() == 4 || digits.size() == 6 || digits.size() == 8) && AllOf(digits, IsHexDigit);
    }
    return !text.empty() && text.size() <= 32 && AllOf(text, IsLetter);
}

std::vector<BlockIssue> ValidateBlock(const ContentBlock& b, bool requireId) {
    std::vector<BlockIssue> issues;
    auto bad = [&](std::string message) { issues.push_back({ b.id, std::move(message) }); };

    if (b.id.empty()) {
        if (requireId) bad("a block needs an id");
    } else if (b.id.size() > kMaxIdLength || !AllOf(b.id, IsPrintable)) {
        bad(std::format("the id must be 1-{} printable characters", kMaxIdLength));
    }
    if (b.kind.empty() || b.kind.size() > kMaxKindLength || !AllOf(b.kind, IsKindChar))
        bad(std::format("the kind must be 1-{} letters, digits, '_' or '-'", kMaxKindLength));

    if (!InRange(b.x, -kMaxPosition, kMaxPosition) || !InRange(b.y, -kMaxPosition, kMaxPosition))
        bad(std::format("x and y must be finite numbers within +-{:.0f}", kMaxPosition));
    if (!InRange(b.width, 0, kMaxSize) || !InRange(b.height, 0, kMaxSize))
        bad(std::format("width and height must be finite numbers from 0 to {:.0f}", kMaxSize));

    if (b.text.size() > kMaxTextLength) bad(std::format("the text is longer than {} characters", kMaxTextLength));

    const ContentStyle& s = b.style;
    if (!InRange(s.padding, 0, kMaxStyleValue) || !InRange(s.cornerRadius, 0, kMaxStyleValue) || !InRange(s.borderWidth, 0, kMaxStyleValue))
        bad(std::format("padding, corner radius and border width must be finite numbers from 0 to {:.0f}", kMaxStyleValue));
    if (!IsColorText(s.backgroundColor)) bad(std::format("'{}' is not a colour (background)", s.backgroundColor));
    if (!IsColorText(s.borderColor)) bad(std::format("'{}' is not a colour (border)", s.borderColor));
    if (s.borderStyle != "line" && s.borderStyle != "dotted" && s.borderStyle != "dashed")
        bad(std::format("the border style must be line, dotted or dashed, not '{}'", s.borderStyle));

    if (b.metaJson.size() > kMaxMetaLength) {
        bad(std::format("the block's settings are longer than {} characters", kMaxMetaLength));
    } else if (auto meta = json::Parse(b.metaJson); !meta.ok() || !meta.value().asObject()) {
        bad("the block's settings must be a JSON object");
    }

    if (!b.bind.empty() && (b.bind.size() > kMaxBindLength || !AllOf(b.bind, IsBindChar)))
        bad(std::format("the bound field name must be 1-{} letters, digits, '_', '-' or '.'", kMaxBindLength));
    return issues;
}

std::vector<BlockIssue> ValidateBlocks(const std::vector<ContentBlock>& blocks) {
    std::vector<BlockIssue> issues;
    if (blocks.size() > kMaxBlocksPerList)
        issues.push_back({ {}, std::format("there are {} blocks; at most {} are allowed", blocks.size(), kMaxBlocksPerList) });
    std::set<std::string_view> seen;
    for (const ContentBlock& b : blocks) {
        for (BlockIssue& issue : ValidateBlock(b, /*requireId=*/true)) issues.push_back(std::move(issue));
        if (!b.id.empty() && !seen.insert(b.id).second) issues.push_back({ b.id, "another block has the same id" });
    }
    return issues;
}

namespace {

Result<void> Report(const std::vector<BlockIssue>& issues) {
    if (issues.empty()) return {};
    const BlockIssue& first = issues.front();
    std::string message = first.blockId.empty() ? first.message : std::format("block '{}': {}", first.blockId, first.message);
    if (issues.size() > 1) message += std::format(" (and {} more)", issues.size() - 1);
    return Error::Make(Err::InvalidArgument, kModule, std::move(message));
}

} // namespace

Result<void> CheckBlock(const ContentBlock& block, bool requireId) { return Report(ValidateBlock(block, requireId)); }

Result<void> CheckBlocks(const std::vector<ContentBlock>& blocks) { return Report(ValidateBlocks(blocks)); }

} // namespace bps::presentation
