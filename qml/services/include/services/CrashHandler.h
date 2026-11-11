#pragma once

#include <QString>

void InstallCrashHandler();

QString ConsumePendingCrashSummary();

void WriteHangDump(const char *reason);

// Always-on diagnostic (BPS_GUI_STALL_MS=<ms>, default 100, 0 = off): a sampler thread that, whenever the interface thread has not ticked
// for that long, captures WHERE it is (module+offset frames) a few times during the stall and logs them.
void StartGuiStallSampler(int thresholdMs);

// Opt-in (BPS_LIVE_PROFILE=1, Windows): samples the live compose thread every ~3 ms and logs, every 15 s, the most
// frequent stacks (module+offset frames, resolve with gdb `info symbol`). Where the audience frame's time actually goes.
void StartLoopProfiler();

void VerifyForensicsTooling();