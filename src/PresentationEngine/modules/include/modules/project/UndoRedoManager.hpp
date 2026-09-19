#pragma once

// UndoRedoManager (docs/specs/15 §Undo/Redo Manager): command-based undo/redo
// with transaction grouping. Every editing operation is an ICommand; the
// manager owns the two stacks and the history limit. Purely in-memory (the
// HistoryManager persists checkpoints).

#include "core/common/Common.hpp"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

// One reversible editing operation (docs/specs/15 §Command-based undo).
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void Execute() = 0;   // called once by ExecuteCommand
    virtual void Undo() = 0;
    virtual void Redo() { Execute(); }
    virtual std::string Label() const { return "command"; }
};

// Simple label-only command used by tests and modules that manage their own
// state; executes a pair of lambdas.
class LambdaCommand final : public ICommand {
public:
    LambdaCommand(std::string label, std::function<void()> doFn, std::function<void()> undoFn)
        : label_(std::move(label)), do_(std::move(doFn)), undo_(std::move(undoFn)) {}
    void Execute() override { if (do_) do_(); }
    void Undo() override { if (undo_) undo_(); }
    std::string Label() const override { return label_; }

private:
    std::string label_;
    std::function<void()> do_;
    std::function<void()> undo_;
};

class UndoRedoManager {
public:
    static UndoRedoManager& Instance() {
        static UndoRedoManager instance;
        return instance;
    }

    UndoRedoManager() = default;

    // Execute a command and push it onto the undo stack (clears redo).
    Result<void> ExecuteCommand(std::shared_ptr<ICommand> cmd);
    // Group marker: commands executed between BeginGroup/EndGroup undo/redo
    // together as one transaction.
    Result<void> BeginGroup(std::string_view label);
    Result<void> EndGroup();

    Result<void> Undo();
    Result<void> Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    std::string UndoLabel() const;
    std::string RedoLabel() const;

    void SetLimit(size_t limit);
    size_t Depth() const;             // undo stack depth
    void Clear();

private:
    struct Entry {
        std::string label;
        std::vector<std::shared_ptr<ICommand>> commands;   // one for plain cmds, N for groups
    };
    void PushUndo(Entry e);
    Entry PopUndo();
    Entry PopRedo();

    mutable std::mutex mutex_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    size_t limit_ = 1000;
    // Active group tracking (guarded by mutex_).
    std::vector<Entry>* group_ = nullptr;
    std::vector<Entry> pendingGroup_;
    bool grouping_ = false;
};

} // namespace bps::project
