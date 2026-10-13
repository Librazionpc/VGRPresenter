#pragma once

#include <QtGlobal>

#include <algorithm>

// THE reopen policy for a closed channel — one definition, shared by every
// model that stores a per-side trim (AudioInputListModel, BusListModel,
// VideoSourceListModel), because the SAME control closes a side on all three:
// the Media pane's L/R pill, the Settings dialogs' gain knob, the monitor
// strips. Before this, each surface answered "what does it come back at?"
// differently: the dialogs remembered the trim for the life of one open dialog
// and reopened at it, while every pill reopened its side at UNITY — so a side
// closed at −6 dB came back at 0 dB when you clicked it in the Media pane, and
// the same click in the dialog came back at −6 dB.
//
// The three answers it keeps in step:
//   * closing writes gain 0 (the app's convention — every meter reads 0 as off,
//     see Db.isChannelOff in qml/settings/Db.qml, whose floor is this one);
//   * the TRIM a side was closed with is remembered separately, so it survives
//     the close (it is the value that used to be thrown away);
//   * reopening restores that trim, and only falls back to unity when there
//     genuinely is none.
//
// Kept deliberately arithmetic-only: no Qt classes, no engine types, so the
// models' own copy of a trim list is the only state involved.
namespace channeltrim {

// A gain at or below this IS "closed". The mirror of Db.floorLinear — the two
// must stay in step or a side can read as closed on screen and open in the
// mixer.
inline constexpr qreal kOffFloor = 0.0005;

inline bool isOff(qreal gain)
{
    return gain <= kOffFloor;
}

// The tone a side is closed to, and the one it is open at.
inline qreal closed() { return 0.0; }
inline qreal unity() { return 1.0; }

// The gain a side comes back at when it is reopened:
//   the trim it was closed with, else
//   whatever level it is holding open right now, else
//   unity — never the 0 that "closed" is stored as.
inline qreal reopenGain(qreal rememberedTrim, qreal currentGain)
{
    if (!isOff(rememberedTrim))
        return std::clamp(rememberedTrim, 0.0, 1.0);
    if (!isOff(currentGain))
        return std::clamp(currentGain, 0.0, 1.0);
    return unity();
}

// The trim to REMEMBER for a gain being written: any open level is a trim; a
// closed one is not (it would arm the next reopen with silence).
inline bool remembersTrim(qreal gain)
{
    return !isOff(gain);
}

} // namespace channeltrim
