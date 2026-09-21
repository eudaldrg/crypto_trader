#include "feed_handler/journal_thread.h"

#include <algorithm>
#include <utility>

namespace feed_handler {

JournalThread::JournalThread(Config cfg)
    : ring_(std::max<std::size_t>(cfg.ring_events, 1)),
      capacity_(std::max<std::size_t>(cfg.ring_events, 1)),
      on_fatal_(std::move(cfg.on_fatal)) {
    if (cfg.start) {
        Start();
    }
}

JournalThread::~JournalThread() {
    Start();
    ring_.PushControl(JournalStop{});
    Wake(true);
    thread_.join();
}

void JournalThread::Start() {
    if (!thread_.joinable()) {
        thread_ = std::thread(&JournalThread::Run, this);
    }
}

void JournalThread::Connect(std::unique_ptr<JournalWriter> writer, const CaptureFrame& marker) {
    ring_.PushControl(JournalConnect{.writer = std::move(writer), .marker = CopyFrame(marker)});
    Wake(true);
}

bool JournalThread::Push(const CaptureFrame& frame) {
    if (Failed()) {
        return false;
    }
    if (!ring_.Push(JournalEvent(CopyFrame(frame)))) {
        Fail("journal ring overflow (" + std::to_string(capacity_) + " events queued)");
        return false;
    }
    Wake(false);
    return true;
}

std::uint64_t JournalThread::Disconnect() {
    Start();  // A barrier must not wait on a thread that is not running.
    auto signal = std::make_shared<JournalCloseSignal>();
    ring_.PushControl(JournalDisconnect{.signal = signal});
    Wake(true);
    return signal->Wait();
}

void JournalThread::Wake(bool must) {
    if (!must && !sleeping_.load()) {
        return;
    }
    {
        // Taking the mutex is what orders this after the journal thread either
        // having seen the event (it re-checks the ring under this mutex before it
        // waits) or being in wait() already; a bare notify could land in between
        // and be lost. Same reasoning as StopSignal::RequestStop.
        const std::lock_guard<std::mutex> lock(wake_mutex_);
    }
    wake_cv_.notify_one();
}

void JournalThread::WaitForEvent(JournalEvent& event) {
    if (ring_.Pop(event)) {
        return;
    }
    std::unique_lock<std::mutex> lock(wake_mutex_);
    sleeping_.store(true);
    while (!ring_.Pop(event)) {
        wake_cv_.wait_for(lock, kIdlePoll);
    }
    sleeping_.store(false);
}

void JournalThread::Fail(std::string_view reason) {
    int expected = kHealthy;
    if (!state_.compare_exchange_strong(expected, kClaimed)) {
        return;  // Already failed, or another thread is recording the failure.
    }
    error_.assign(reason);
    state_.store(kFailed, std::memory_order_release);
    if (on_fatal_) {
        on_fatal_(error_);
    }
}

void JournalThread::AfterWrite() {
    records_written_.store(writer_->RecordsWritten(), std::memory_order_relaxed);
    if (!writer_->Good()) {
        Fail(writer_->Error());
    }
}

void JournalThread::Run() {
    JournalEvent event;
    for (;;) {
        WaitForEvent(event);
        if (auto* frame = std::get_if<FrameEvent>(&event)) {
            if (writer_ != nullptr) {
                writer_->OnFrame(frame->View());
                AfterWrite();
            }
        } else if (auto* connect = std::get_if<JournalConnect>(&event)) {
            writer_ = std::move(connect->writer);
            writer_->WriteConnectMarker(connect->marker.View());
            AfterWrite();
        } else if (auto* disconnect = std::get_if<JournalDisconnect>(&event)) {
            std::uint64_t records = 0;
            if (writer_ != nullptr) {
                writer_->Flush();
                AfterWrite();
                records = writer_->RecordsWritten();
                writer_.reset();
            }
            records_written_.store(0, std::memory_order_relaxed);
            disconnect->signal->Complete(records);
        } else {
            // JournalStop. Destroying the writer flushes it.
            writer_.reset();
            return;
        }
    }
}

}  // namespace feed_handler
