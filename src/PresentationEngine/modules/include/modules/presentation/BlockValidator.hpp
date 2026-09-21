#pragma once

// BlockValidator: the ENGINE's rules for what a content block may be. Every place blocks enter the engine - a slide's content
// flushed from the Edit screen, a block added or replaced, a template's or an overlay's blocks, a design saved in the overlay /
// template library - is checked here, so the UI can never leave the engine holding a block that another part of it cannot
// handle (a duplicate id, a NaN width, a colour nothing can read).
//
// The rules are about the block being SOUND, not about how it looks:
//   id ......... unique within its list, 1-80 printable characters (an added block has none yet: the engine claims one)
//   kind ....... 1-40 of letters, digits, '_' and '-' (the set of kinds is open: "text", "shape", "vignette" ...)
//   geometry ... x, y, width, height are finite numbers; x and y within +-100000, width and height within 0..100000
//   text ....... at most 200000 characters
//   style ...... padding, cornerRadius, borderWidth finite and within 0..10000; backgroundColor and borderColor are
//                "transparent", "#RGB", "#ARGB", "#RRGGBB", "#AARRGGBB" or a colour name (letters only);
//                borderStyle is "line", "dotted" or "dashed"
//   meta ....... a JSON object, at most 100000 characters
//   bind ....... empty, or a field name (letters, digits, '_', '-', '.', at most 40)
//   list ....... at most 500 blocks
// A rejected block is never repaired silently: the caller gets InvalidArgument saying which block and what is wrong, and the
// engine's content stays exactly as it was.

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
