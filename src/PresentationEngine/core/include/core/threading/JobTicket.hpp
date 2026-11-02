#pragma once

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
        return !abandoned_.load(std::memory_order_acquire);
    }

    bool Deliver() const
    {
        return !withdrawn_.load(std::memory_order_acquire)
               && !abandoned_.load(std::memory_order_acquire);
    }

    bool Started() const { return started_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> abandoned_{false};
    std::atomic<bool> withdrawn_{false};
    std::atomic<bool> started_{false};
};

using JobTicketPtr = std::shared_ptr<JobTicket>;
} // namespace bps