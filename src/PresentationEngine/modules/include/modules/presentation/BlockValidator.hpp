#pragma once

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bps::presentation {
inline constexpr size_t kMaxBlocksPerList = 500;

struct BlockIssue {
    std::string blockId;   // "" when the block has no id yet
    std::string message;   // what is wrong, in words ("width must be a finite number from 0 to 100000")
};

// True for "transparent", a hex colour ("#RGB", "#ARGB", "#RRGGBB", "#AARRGGBB") or a colour name.
bool IsColorText(std::string_view text);

// Every problem with one block. `requireId`: false for a block being ADDED (the engine gives it its id).
std::vector<BlockIssue> ValidateBlock(const ContentBlock& block, bool requireId);
// Every problem with a whole list (each block, plus duplicate ids and the size of the list).
std::vector<BlockIssue> ValidateBlocks(const std::vector<ContentBlock>& blocks);

// OK, or InvalidArgument naming the first problem (and how many more there are).
Result<void> CheckBlock(const ContentBlock& block, bool requireId = false);
Result<void> CheckBlocks(const std::vector<ContentBlock>& blocks);
} // namespace bps::presentation