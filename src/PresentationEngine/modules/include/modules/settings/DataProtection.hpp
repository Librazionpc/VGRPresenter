#pragma once

// Settings > General > Backups & recovery, as the engine does it.
//
//   BackupStore    dated copies of a show file in one folder, newest kept: "<show>__<yyyymmdd-hhmmss>.<ext>". Create() copies a
//                  file in, Prune() drops all but the newest N of that show, List() reads them back (newest first).
//   RecoveryStore  where an unexpected exit's unsaved work is kept: the app writes the open show to Path() while it has
//                  unsaved changes, and Clear()s it on a clean save or exit. If Info() finds one on the next start, the
//                  last run did not end cleanly and there is something to restore.
//
// The app decides WHEN (timers, the show being dirty); how a file is named, copied, listed and pruned is settled here.

#include "core/common/Common.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bps::settings {

struct BackupInfo {
    std::string path;
    std::string show;      // the show's file name without extension
    std::string stamp;     // yyyymmdd-hhmmss
    uint64_t sizeBytes = 0;
};

// The local time as "yyyymmdd-hhmmss".
std::string NowStamp();

class BackupStore {
public:
    explicit BackupStore(std::string directory) : directory_(std::move(directory)) {}

    // Copies `sourceFile` into the folder as a dated backup and returns the backup's path. `stamp` is the moment it is
    // stamped with (the current time when empty). Two backups within the same second get "-2", "-3"... so none is overwritten.
    Result<std::string> Create(std::string_view sourceFile, std::string stamp = {}) const;

    // Like Create(), but does nothing (returns "") when the newest backup of this show is already an exact copy of the
    // file - so a timer can ask every few minutes without piling up identical copies of an unchanged show.
    Result<std::string> CreateIfChanged(std::string_view sourceFile, std::string stamp = {}) const;

    // Every backup, newest first; `show` (a file name without extension) narrows it to that show's own.
    std::vector<BackupInfo> List(std::string_view show = {}) const;

    // Keeps the newest `keepLast` backups OF THAT SHOW and removes the rest. Returns how many were removed.
    size_t Prune(std::string_view show, size_t keepLast) const;

    const std::string& Directory() const noexcept { return directory_; }

private:
    std::string directory_;
};

struct RecoveryInfo {
    std::string path;          // the saved copy of the show that was open
    std::string showName;
    std::string originalPath;  // where that show lived ("" = it had never been saved)
};

class RecoveryStore {
public:
    explicit RecoveryStore(std::string directory) : directory_(std::move(directory)) {}

    // Where the app writes the recovery copy of the open show.
    std::string Path() const;

    // Notes which show the copy belongs to. Call after writing the copy at Path().
    Result<void> Note(std::string_view showName, std::string_view originalPath) const;

    // What is waiting to be restored, or nothing.
    std::optional<RecoveryInfo> Info() const;

    // Copies the waiting recovery copy to `target` (the app then opens it) and clears it. Nothing is overwritten:
    // a `target` that already exists is refused.
    Result<void> RestoreTo(std::string_view target) const;

    // Removes the copy and its note (a clean save or exit, or the user declined the restore).
    Result<void> Clear() const;

private:
    std::string directory_;
};

} // namespace bps::settings
