#pragma once

// ============================================================================
// tools/gui_thread_fixtures/fixture_shapes.h — the shapes whose verdict
// tools/check_gui_thread_blocks.mjs must produce.
//
// TEXT TO THE GUARD, NEVER COMPILED: nothing includes these files and no target
// builds them; they exist so the guard's own rules are checked before every scan
// (its `--self-test`, the shape tools/lock_scope_fixtures established for the
// lock guard — see AGENTS.md). Each class below is one case, and expect.json
// states the verdict it must get. A rule that stops firing fails the build here
// instead of quietly approving everything; a rule that starts firing on work that
// runs on a WORKER fails here too.
//
// The engine primitives are written as real text (`BibleEngine::Instance()`) on
// purpose: the guard is a name-and-shape scan, so that is exactly what it sees.
// ============================================================================

class QObject;

// ---- CASE 1: the helper reads the engine ON A WORKER ----------------------
// The read sits inside the thread's own lambda, so it is not the caller's
// thread. Before the guard learned to look inside a hop's argument region this
// shape labelled EVERY caller that queued it, which is the false finding that
// made a published cache (read on a worker) look like a blocking call.
// EXPECTED: no finding at all.
class FixtureHopHelper : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE QString hopCaller();
private:
    QString workerRead() const;
};

// ---- CASE 2: the helper reads the engine INLINE ---------------------------
// The same call on the caller's thread, so the entry point that reaches it is
// still reported — the span rule must not swallow a real wait.
// EXPECTED: soft FixtureInlineHelper::inlineCaller.
class FixtureInlineHelper : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE QString inlineCaller();
private:
    QString immediateRead() const;
};

// ---- CASE 3: the entry point hands its device work over, then reads --------
// Bookkeeping here plus a queued job is the codebase's fix shape
// (`openTapOffThread`, the twelve cleared in the audit's §6.5): the interface
// thread does the roster/state work and the engine call is posted.
// EXPECTED: exempt (no finding), and counted as already off-thread.
class FixtureHopEntry : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void hopEntry();
};

// ---- CASE 4: a waiver with no reason ---------------------------------------
// AGENTS.md: "a bare `gui-ok:` is itself a finding". Reported as such, and now
// gating like the stale-waiver rule one step later.
// EXPECTED: a finding (labels: gui-ok without a reason) AND the bare list.
class FixtureBareWaiver : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE QString bareWaiver();   // gui-ok:
};

// ---- CASE 5: a reasoned waiver on a call that no longer blocks ------------
// The reason is mandatory and the liveness check is what stops a waiver rotting:
// an exemption that no longer trips a rule is reported STALE.
// EXPECTED: STALE FixtureStaleWaiver::staleWaiver, and no finding.
class FixtureStaleWaiver : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE QString staleWaiver();   // gui-ok: nothing behind it any more — this call reads this object's own members
};

// ---- CASE 9: the engine read sits inside a POOL lambda ---------------------
// The other posting shape this codebase uses, and the one the span rule used to
// get wrong: the first `(` after `QThreadPool` belongs to `globalInstance()`, so
// the lambda that actually runs on a pool thread sat OUTSIDE the span and its
// engine read counted as work on the caller's thread.
// EXPECTED: no finding at all.
class FixturePoolHelper : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void poolCaller();
private:
    void poolRead();
};

// ---- CASE 6: the helper POSTS its device work, then returns ----------------
// The same reason an entry point is exempt when it hops, applied one level down;
// the singleton read on the caller's thread is what the rule has to weigh, and
// the verdict is "no chain to report" — the caller is not waiting on the device.
// Without it every command that reached such a helper read as blocking
// (armNdiSenderForSession's queued SDK reset, refreshNdiConnections' pool job).
// EXPECTED: no finding at all (its caller inherits nothing).
class FixturePostingHelper : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void postsCaller();
private:
    void armSender();
};

// ---- CASE 7: an empty QImage() is not a data load --------------------------
// `lastFrame = QImage();` — a reset, not a decode. The rule used to read the
// bare `QImage(` and this one spelling kept clearMedia() in the inventory.
// EXPECTED: no finding.
class FixtureEmptyImage : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void resetFrame();
private:
    QImage lastFrame_;
    bool clearFlag_ = false;
};

// ---- CASE 8: a QImage loaded FROM A PATH is a data load --------------------
// The other half of CASE 7: narrowing the rule must not swallow a real decode.
// EXPECTED: soft FixtureImageLoad::loadFrame.
class FixtureImageLoad : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void loadFrame();
private:
    QString pathAt(int index) const;
    QImage lastFrame_;
};
