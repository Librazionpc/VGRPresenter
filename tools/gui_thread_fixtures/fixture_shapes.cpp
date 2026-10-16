#include "fixture_shapes.h"

#include <QMetaObject>
#include <QThread>

// ============================================================================
// The bodies behind tools/gui_thread_fixtures/fixture_shapes.h — see that file
// for what each one proves. Never compiled; the guard reads the text.
// ============================================================================

// ---- CASE 1: read on a worker (must stay QUIET) ----------------------------
QString FixtureHopHelper::workerRead() const
{
    QString first;
    QThread *runner = QThread::create([&first]() {
        auto ids = bps::bible::BibleEngine::Instance().BibleIds();
        if (!ids.empty())
            first = QString::fromStdString(ids.front());
    });
    runner->start();
    runner->wait();
    return first;
}

QString FixtureHopHelper::hopCaller()
{
    const QString name = workerRead();
    return name;
}

// ---- CASE 2: read inline (must be REPORTED) --------------------------------
QString FixtureInlineHelper::immediateRead() const
{
    auto ids = bps::bible::BibleEngine::Instance().BibleIds();
    return ids.empty() ? QString() : QString::fromStdString(ids.front());
}

QString FixtureInlineHelper::inlineCaller()
{
    // The same helper shape as CASE 1, one call away from the interface thread:
    // this is the finding the guard exists for (a lock the capture worker holds,
    // taken between a click and the next paint).
    return immediateRead();
}

// ---- CASE 3: hop in the entry point's own body -----------------------------
void FixtureHopEntry::hopEntry()
{
    // The bookkeeping a fixed entry point is allowed to keep on the interface
    // thread — here a singleton read with the engine call posted below it, so
    // there IS something reachable for the rules to see. Without a listed
    // spelling the rule's reach is what this case would be measuring, which is
    // why the shape matters: the real fixed entry points read a roster or an
    // owner table and post the device work, and it is the POST that exempts.
    (void)bps::bible::BibleEngine::Instance().BibleIds();
    const bool alive = EngineBridge::instance().inputSourceAlive(0);
    (void)alive;
    // … and the device call it hands to the worker, which is what makes this
    // entry point off-thread by construction rather than by a waiver.
    vgr::EngineWorker::instance().runOffThread([]() {
        (void)bps::bible::BibleEngine::Instance().BibleIds();
    });
}

// ---- CASE 4: a bare waiver over a real read --------------------------------
QString FixtureBareWaiver::bareWaiver()
{
    auto ids = bps::bible::BibleEngine::Instance().BibleIds();
    return ids.empty() ? QString() : QString::fromStdString(ids.front());
}

// ---- CASE 5: a reasoned waiver over nothing ---------------------------------
QString FixtureStaleWaiver::staleWaiver()
{
    return QStringLiteral("this object's own member, read on this thread");
}

// ---- CASE 9: a pool lambda (must stay QUIET) -------------------------------
void FixturePoolHelper::poolRead()
{
    QThreadPool::globalInstance()->start([]() {
        (void)bps::bible::BibleEngine::Instance().BibleIds();
    });
}

void FixturePoolHelper::poolCaller()
{
    poolRead();
}

// ---- CASE 6: the helper posts, the caller does not wait ---------------------
void FixturePostingHelper::armSender()
{
    // A provider lookup on this thread — the cheap half …
    auto *provider = bps::display::DisplayEngine::Instance().Provider("Ndi");
    if (!provider)
        return;
    // … and the device call handed over, which is what makes the CALLER safe.
    QMetaObject::invokeMethod(this, [provider]() { provider->ResetSender(); },
                              Qt::QueuedConnection);
}

void FixturePostingHelper::postsCaller()
{
    armSender();
}

// ---- CASE 7: the empty construction (must stay QUIET) -----------------------
void FixtureEmptyImage::resetFrame()
{
    lastFrame_ = QImage();
    clearFlag_ = true;
}

// ---- CASE 8: a real decode (must be REPORTED) ------------------------------
void FixtureImageLoad::loadFrame()
{
    lastFrame_ = QImage(pathAt(0));
}
