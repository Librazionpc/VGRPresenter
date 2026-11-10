#pragma once

#include <QString>

void InstallCrashHandler();

QString ConsumePendingCrashSummary();

void WriteHangDump(const char *reason);

// Always-on diagnostic (BPS_GUI_STALL_MS=<ms>, default 100, 0 = off): a sampler thread that, whenever the interface thread has not ticked
// for that long, captures WHERE it is (module+offset frames) a few times during the stall and logs them.
void StartGuiStallSampler(int thresholdMs);

void VerifyForensicsTooling();