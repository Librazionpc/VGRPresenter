#include "modules/project/UndoRedoManager.hpp"

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"

namespace bps::project {

Result<void> UndoRedoManager::ExecuteCommand(std::shared_ptr<ICommand> cmd) {
    if (!cmd)
        return Error::Make(Err::InvalidArgument, "UndoRedo", "null command");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (grouping_) {
            if (pendingGroup_.empty()) pendingGroup_.push_back(Entry{});
            if (pendingGroup_.back().label.empty())
                pendingGroup_.back().label = cmd->Label();
            pendingGroup_.back().commands.push_back(std::move(cmd));
            return Ok();
        }
    }
    // Not grouping: run outside the lock, then push.
    cmd->Execute();
    Entry e;
    e.label = cmd->Label();
    std::string label = e.label;   // copy before the move (use-after-move guard)
    e.commands.push_back(std::move(cmd));
    PushUndo(std::move(e));
    (void)EventBus::Instance().Publish(events::UndoPerformed{label, Depth()});
    return Ok();
}

Result<void> UndoRedoManager::BeginGroup(std::string_view label) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (grouping_)
        return Error::Make(Err::InvalidState, "UndoRedo", "group already active");
    grouping_ = true;
    pendingGroup_.clear();
    pendingGroup_.push_back(Entry{std::string(label), {}});
    return Ok();
}

Result<void> UndoRedoManager::EndGroup() {
    Entry group;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!grouping_) return Ok();
        grouping_ = false;
        if (pendingGroup_.empty() || pendingGroup_.back().commands.empty()) {
            pendingGroup_.clear();
            return Ok();
        }
        group = std::move(pendingGroup_.back());
        pendingGroup_.clear();
    }
    // Execute the whole group now that it is complete.
    for (auto& c : group.commands) c->Execute();
    PushUndo(std::move(group));
    (void)EventBus::Instance().Publish(events::UndoPerformed{group.label, Depth()});
    return Ok();
}

void UndoRedoManager::PushUndo(Entry e) {
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.push_back(std::move(e));
    if (undo_.size() > limit_)
        undo_.erase(undo_.begin(), undo_.begin() + static_cast<long>(undo_.size() - limit_));
    redo_.clear();
}

UndoRedoManager::Entry UndoRedoManager::PopUndo() {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    return e;
}

UndoRedoManager::Entry UndoRedoManager::PopRedo() {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    return e;
}

Result<void> UndoRedoManager::Undo() {
    Entry e;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (undo_.empty())
            return Error::Make(Err::Project_UndoEmpty, "UndoRedo", "nothing to undo");
        e = std::move(undo_.back());
        undo_.pop_back();
    }
    // Undo in reverse order (groups undo their last command first).
    for (auto it = e.commands.rbegin(); it != e.commands.rend(); ++it) (*it)->Undo();
    std::lock_guard<std::mutex> lock(mutex_);
    redo_.push_back(std::move(e));
    return Ok();
}

Result<void> UndoRedoManager::Redo() {
    Entry e;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (redo_.empty())
            return Error::Make(Err::Project_RedoEmpty, "UndoRedo", "nothing to redo");
        e = std::move(redo_.back());
        redo_.pop_back();
    }
    for (auto& c : e.commands) c->Redo();
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.push_back(std::move(e));
    return Ok();
}

bool UndoRedoManager::CanUndo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !undo_.empty();
}

bool UndoRedoManager::CanRedo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !redo_.empty();
}

std::string UndoRedoManager::UndoLabel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return undo_.empty() ? std::string{} : undo_.back().label;
}

std::string UndoRedoManager::RedoLabel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return redo_.empty() ? std::string{} : redo_.back().label;
}

void UndoRedoManager::SetLimit(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    limit_ = limit == 0 ? 1 : limit;
    if (undo_.size() > limit_)
        undo_.erase(undo_.begin(), undo_.begin() + static_cast<long>(undo_.size() - limit_));
}

size_t UndoRedoManager::Depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return undo_.size();
}

void UndoRedoManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    undo_.clear();
    redo_.clear();
    grouping_ = false;
    pendingGroup_.clear();
}

} // namespace bps::project
