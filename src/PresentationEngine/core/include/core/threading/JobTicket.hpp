#pragma once

// JobTicket — the cancellation handle on a job that was handed to another thread.
//
// Why this exists. Everything the QML layer hands off runs on ONE serialized
// worker thread, in the order it was posted (see EngineWorker: order is the
// contract, so a slider drag's twenty gain changes reach the engine in order).
// The price of a single queue is that a job nobody wants any more still sits in
// it, and still runs to its verdict: an operator who closes a screen or opens
// another show leaves the read they asked for queued ahead of the work the
// screen that IS showing needs, and its verdict arrives later for a caller that
// has to remember a token and ignore it.
//
// The domain has TWO different things to abandon, and they are deliberately
// separate calls, because conflating them loses data:
//
//   * Abandon()          — the WORK is pointless now. A job still queued never
//                          runs (Begin() answers false). One already running
//                          cannot be interrupted mid-engine-call (the calls are
//                          not cooperative — a locked engine write has no safe
//                          cancellation point), so its answer is dropped instead.
//                          For a read this is the win: the disk is never touched.
//   * WithdrawVerdict()  — the ANSWER is pointless now, but the work is not. A
//                          save the operator asked for must still reach the file
//                          (that write IS what they asked for) while the toast and
//                          the completion signal for a screen that is gone must
//                          not be delivered to it.
//
// Both are safe from any thread at any point in a job's life, and the first call
// wins. The worker asks the ticket exactly twice: Begin() before it does anything,
// Deliver() before its answer is used (see EngineWorker::runOffThreadCancellable,
// which asks Begin() itself so a caller cannot forget it).
//
// The state is three atomics and no lock: the poster is usually the interface
// thread and the worker is always another one, so neither may ever wait on the
// other. A ticket is shared (shared_ptr) because the poster keeps a copy to
// cancel with while the worker holds the one it captures by value.

#include <atomic>
#include <memory>

namespace bps {

class JobTicket
{
public:
    // ---- the poster's side -------------------------------------------------

    // Abandons the work: a job still queued is skipped entirely. True when this
    // call is what abandoned it (a second call changes nothing and answers false).
    bool Abandon() { return !abandoned_.exchange(true, std::memory_order_acq_rel); }

    // Abandons only the verdict: the work still runs, and whatever it changed for
    // real (the file it wrote) still stands; the answer is not delivered.
    bool WithdrawVerdict() { return !withdrawn_.exchange(true, std::memory_order_acq_rel); }

    // True once either was called: nobody is waiting for this job any more. This
    // is what a waiter checks to stop waiting instead of hanging on a verdict that
    // is never coming.
    bool Cancelled() const
    {
        return abandoned_.load(std::memory_order_acquire)
               || withdrawn_.load(std::memory_order_acquire);
    }

    // ---- the worker's side -------------------------------------------------

    // May this job do its work at all? False = abandoned while queued; nothing
    // will come of the work, so it must not be done.
    bool Begin()
    {
        if (abandoned_.load(std::memory_order_acquire))
            return false;
        started_.store(true, std::memory_order_release);
        // Re-checked after marking it started: an Abandon() that lands in between
        // still means "too late to skip", but a cancel that was already in flight
        // must not let the work run anyway. Either answer is correct — the marker
        // exists so Deliver() can tell "never ran" from "ran and was dropped".
        return !abandoned_.load(std::memory_order_acquire);
    }

    // May this job's answer be used? False = the verdict was abandoned after the
    // work began (or began and was dropped), so publishing it would report
    // something to a caller that has stopped listening.
    bool Deliver() const
    {
        return !withdrawn_.load(std::memory_order_acquire)
               && !abandoned_.load(std::memory_order_acquire);
    }

    // Did the work actually run? The distinction that matters after a cancel: a
    // job that ran may have changed something permanent (a file), and those
    // consequences are not part of the withdrawn verdict.
    bool Started() const { return started_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> abandoned_{false};
    std::atomic<bool> withdrawn_{false};
    std::atomic<bool> started_{false};
};

using JobTicketPtr = std::shared_ptr<JobTicket>;

} // namespace bps
