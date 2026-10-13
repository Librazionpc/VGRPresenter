// ============================================================================
// tools/channel_trim_check.cpp — does the ONE closed-channel reopen policy do
// what the three models (AudioInputListModel, BusListModel,
// VideoSourceListModel) compile against it for?
//
// WHY THIS EXISTS: the reported bug was a pane handing a closed side back at
// UNITY — the trim it was closed with was thrown away, and each surface had its
// own copy of `closed ? 1.0 : 0.0`. The fix is one header
// (qml/models/include/models/ChannelTrim.h) that all three models and both
// dialogs' declarations go through. It is pure arithmetic — no Qt classes, no
// device, no graph, no window — so it can be checked exactly and instantly,
// where the failure it prevents (a side reopening at the wrong level) otherwise
// only shows up by clicking a pill on a running app.
//
// Run it with tools/check_channel_trim.sh (prints CHANNEL_TRIM: PASS/FAIL).
// ============================================================================
#include "ChannelTrim.h"

#include <cstdio>

static int failures = 0;

static void check(const char *what, bool ok)
{
    std::printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        ++failures;
}

int main()
{
    using namespace channeltrim;

    // What "closed" means. This floor is the mirror of Db.floorLinear
    // (qml/settings/Db.qml): if one moves the other must follow, or a side reads
    // closed on screen and open in the mixer.
    check("0 reads as closed", isOff(0.0));
    check("the floor itself reads as closed", isOff(kOffFloor));
    check("a hair above the floor is open", !isOff(0.001));

    // The reopen policy itself.
    check("no trim at all reopens at UNITY", reopenGain(0.0, 0.0) == 1.0);
    check("THE headline case: closed at 0.5 reopens at 0.5 (-6 dB)",
          reopenGain(0.5, 0.0) == 0.5);
    check("a remembered trim wins over whatever the side now holds",
          reopenGain(0.5, 0.9) == 0.5);
    check("with no trim, the level the side holds is used", reopenGain(0.0, 0.7) == 0.7);
    check("a stored 0 can never come back as silence", reopenGain(0.0, 0.0) != 0.0);
    check("an over-unity trim is clamped, not passed through", reopenGain(1.5, 0.0) == 1.0);
    check("a negative trim reads as none, not as silence", reopenGain(-1.0, 0.0) == 1.0);

    // What is worth remembering.
    check("an open level IS remembered as a trim", remembersTrim(0.5));
    check("unity IS remembered as a trim", remembersTrim(1.0));
    check("a close is NOT remembered as a trim", !remembersTrim(0.0));

    std::printf("%s\n", failures == 0 ? "CHANNEL_TRIM: PASS" : "CHANNEL_TRIM: FAIL");
    return failures == 0 ? 0 : 1;
}
