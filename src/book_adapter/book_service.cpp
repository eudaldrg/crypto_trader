#include "book_adapter/book_service.h"

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "feed_handler/logging.h"

namespace book_adapter {

namespace {

// Empty passes to poll straight through, then to yield between, before sleeping.
// A pass over a few empty rings costs tens of nanoseconds, so the spin phase is
// microseconds long: enough that a steady feed never sees the sleep, short enough
// that an idle capture is asleep almost all of the time.
constexpr std::size_t kSpinPasses = 256;
constexpr std::size_t kYieldPasses = 1024;
constexpr std::chrono::microseconds kIdleSleep{100};

}  // namespace

BookService::BookService(const Config& config) : config_(config), adapter_(config.adapter) {}

BookService::~BookService() {
    Stop();
}

RingSink& BookService::AddConnection(std::string name, const BookSettings& settings) {
    const ConnectionHandle handle = adapter_.AddConnection(std::move(name), settings);
    feeds_.push_back(std::make_unique<Feed>(config_, handle));
    return feeds_.back()->sink;
}

void BookService::Start() {
    if (feeds_.empty() || thread_.joinable() || stopped_) {
        return;
    }
    thread_ = std::thread(&BookService::Run, this);
}

void BookService::Stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    if (thread_.joinable()) {
        // The store is what publishes every producer's last push to the thread:
        // it reads the flag with acquire BEFORE its final pass, so a pass that
        // finds every ring empty after seeing it has drained everything.
        stop_.store(true, std::memory_order_release);
        thread_.join();
    } else {
        Poll();
    }
    for (const auto& feed : feeds_) {
        ReportTrailingDrops(*feed);
    }
}

std::size_t BookService::Poll() {
    std::size_t total = 0;
    for (std::size_t handled = DrainPass(); handled > 0; handled = DrainPass()) {
        total += handled;
    }
    return total;
}

void BookService::Run() {
    std::size_t idle_passes = 0;
    for (;;) {
        const bool stopping = stop_.load(std::memory_order_acquire);
        if (DrainPass() > 0) {
            idle_passes = 0;
            continue;
        }
        if (stopping) {
            return;
        }
        Backoff(idle_passes);
        ++idle_passes;
    }
}

std::size_t BookService::DrainPass() {
    std::size_t handled = 0;
    for (const auto& feed : feeds_) {
        handled += Drain(*feed);
    }
    return handled;
}

std::size_t BookService::Drain(Feed& feed) {
    RingEntry entry;
    std::size_t handled = 0;
    while (handled < kBatch && feed.ring.Pop(entry)) {
        Dispatch(feed, entry);
        ++handled;
    }
    return handled;
}

void BookService::Dispatch(Feed& feed, const RingEntry& entry) {
    try {
        // The drops this entry knows about happened before it in the stream, so
        // they reach the adapter before it does.
        ReportDrops(feed, entry.dropped_before);
        adapter_.OnEvent(feed.handle, entry.event);
    } catch (const std::exception& error) {
        CountInternalError(feed, error.what());
    } catch (...) {
        CountInternalError(feed, "unknown exception");
    }
}

void BookService::ReportDrops(Feed& feed, std::uint64_t total) {
    if (total <= feed.reported_drops) {
        return;
    }
    const std::uint64_t fresh = total - feed.reported_drops;
    feed.reported_drops = total;
    adapter_.OnFramesDropped(feed.handle, fresh);
}

void BookService::ReportTrailingDrops(Feed& feed) {
    // Every producer has ended, so the counter is final and every entry it
    // stamped has been popped.
    try {
        ReportDrops(feed, feed.ring.Dropped());
    } catch (const std::exception& error) {
        CountInternalError(feed, error.what());
    } catch (...) {
        CountInternalError(feed, "unknown exception");
    }
}

void BookService::CountInternalError(const Feed& feed, const char* what) {
    const std::uint64_t count = internal_errors_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= kMaxLoggedInternalErrors) {
        feed_handler::LogError("[" + adapter_.Name(feed.handle) +
                               "] the book adapter threw and the event was skipped: " + what);
    }
}

void BookService::Backoff(std::size_t idle_passes) {
    if (idle_passes < kSpinPasses) {
        return;
    }
    if (idle_passes < kSpinPasses + kYieldPasses) {
        std::this_thread::yield();
        return;
    }
    std::this_thread::sleep_for(kIdleSleep);
}

std::string BookService::Summary(std::size_t index) const {
    const ConnectionHandle handle = feeds_[index]->handle;
    const ConnectionStats& stats = adapter_.Stats(handle);

    std::string issues;
    for (std::size_t kind = 0; kind < kIntegrityIssueKinds; ++kind) {
        const auto issue = static_cast<order_book::IntegrityIssue>(kind);
        if (stats.IssueCount(issue) != 0) {
            issues += (issues.empty() ? "" : ", ") + std::string(ToString(issue)) + "=" +
                      std::to_string(stats.IssueCount(issue));
        }
    }
    if (issues.empty()) {
        issues = "none";
    }

    return "[" + adapter_.Name(handle) + "] books: " + std::to_string(stats.frames) + " frames, " +
           std::to_string(stats.snapshots) + " snapshots, " + std::to_string(stats.updates) +
           " updates, " + std::to_string(stats.drops) + " dropped, " +
           std::to_string(stats.parse_errors) + " parse errors, " +
           std::to_string(stats.apply_errors) + " apply errors, integrity issues: " + issues +
           ", " + std::to_string(adapter_.BookCount(handle)) + " book(s) at exit";
}

void BookService::LogSummaries() const {
    for (std::size_t index = 0; index < feeds_.size(); ++index) {
        feed_handler::LogInfo(Summary(index));
    }
}

}  // namespace book_adapter
