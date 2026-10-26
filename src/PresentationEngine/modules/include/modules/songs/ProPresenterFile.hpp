#pragma once

// ProPresenter 7/8 presentation documents (.pro).
//
// ProPresenter 7 replaced the human-readable XML of Pro6 with Google protocol
// buffers, so a .pro file is a binary `rv.data.Presentation`: `name`, then
// `cue_groups` (the song's verse/chorus sections, each naming its cues) and
// `cues` (one slide each). A cue's slide text is RTF inside a graphics text
// element — the file's own slide notes are RTF too, and are not lyrics.
//
// This reads that document into the canonical Song model, one cue per section,
// so a ProPresenter library imports like any other song format.
//
// Layout as read here (field numbers confirmed against a real library and the
// reverse-engineered rv.data protos):
//   Presentation        3 name, 7 notes, 12 cue_groups, 13 cues, 14 ccli
//   Presentation.CueGroup  1 group { 1 uuid, 2 name }, 2 repeated uuid
//   Cue                 1 uuid, 2 name, 10 repeated Action
//   Action              9 type, 23 slide { 2 presentation }
//   PresentationSlide   1 base_slide { 1 repeated Element }, 2 notes { 1 rtf }
//   Slide.Element       1 graphics element { 1 uuid, 2 name, 13 text }
//   Graphics.Text       5 rtf_data

#include "core/common/Common.hpp"
#include "modules/songs/SongTypes.hpp"

#include <string_view>

namespace bps::song {

// Parses a ProPresenter 7/8 .pro document. Returns Err::Song_UnsupportedFormat
// with the message the Import dialog shows when the bytes are not a
// presentation at all ("unsupported ProPresenter payload"), and
// Err::Song_ValidationFailed when it is one but holds no slide text.
Result<Song> ParseProPresenterFile(std::string_view content);

} // namespace bps::song
