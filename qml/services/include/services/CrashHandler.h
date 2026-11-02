#pragma once

#include <QString>

void InstallCrashHandler();

QString ConsumePendingCrashSummary();

void WriteHangDump(const char *reason);

void VerifyForensicsTooling();