#pragma once

#include <QList>
#include <QtGlobal>

#include <algorithm>

namespace channeltrim {
inline constexpr qreal kOffFloor = 0.0005;

inline bool isOff(qreal gain)
{
    return gain <= kOffFloor;
}

// The tone a side is closed to, and the one it is open at.
inline qreal closed() { return 0.0; }
inline qreal unity() { return 1.0; }

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

// Grow a per-channel list so `channel` exists (a new entry reads as `absent`), then
// set it. False when it already held that value, so the caller skips its write.
inline bool setChannelValue(QList<qreal> &values, int channel, qreal value, qreal absent)
{
    while (values.size() <= channel)
        values.append(absent);
    if (qFuzzyCompare(values[channel], value))
        return false;
    values[channel] = value;
    return true;
}
} // namespace channeltrim