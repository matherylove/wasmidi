#include "scheduler.hpp"

#include <algorithm>

namespace wasmidi {

MidiScheduler::MidiScheduler() = default;
MidiScheduler::~MidiScheduler() = default;

void MidiScheduler::setDocument(const MidiDocument* doc)
{
    document_ = doc;
    currentTime_ = 0.0f;
    eventCursor_ = 0;
    pendingEvents_.clear();
    rebuildEventStream();
}

void MidiScheduler::setSampleRate(float sampleRate)
{
    sampleRate_ = sampleRate;
}

void MidiScheduler::start()
{
    playing_ = true;
}

void MidiScheduler::pause()
{
    playing_ = false;
}

void MidiScheduler::resume()
{
    playing_ = true;
}

void MidiScheduler::stop()
{
    playing_ = false;
    currentTime_ = 0.0f;
    eventCursor_ = 0;
    pendingEvents_.clear();
}

void MidiScheduler::rebuildEventStream()
{
    events_.clear();
    if (!document_)
        return;

    events_.reserve(document_->notes.size() * 2 + document_->controls.size());

    for (const auto& note : document_->notes) {
        events_.push_back({
            note.startTime, 0x90, note.channel, note.pitch, note.velocity, note.track
        });
        events_.push_back({
            note.endTime, 0x80, note.channel, note.pitch, 0, note.track
        });
    }

    for (const auto& event : document_->controls) {
        events_.push_back({
            event.time, event.type, event.channel, event.data1, event.data2, 0
        });
    }

    std::stable_sort(events_.begin(), events_.end(),
                     [](const ScheduledEvent& a, const ScheduledEvent& b) {
        if (a.time != b.time)
            return a.time < b.time;

        // The legacy player dispatches note-ons in its note cursor before it
        // cleans up sounding note-offs for the same frame. Keep retriggers in
        // that order for parity.
        if (a.type == 0x90 && b.type == 0x80)
            return true;
        if (a.type == 0x80 && b.type == 0x90)
            return false;
        return false;
    });
}

void MidiScheduler::seek(float seconds)
{
    currentTime_ = std::max(0.0f, seconds);

    // MPWGL2 rewinds dispatch by 50 ms after resume/seek so events close to
    // the boundary are not lost.
    const float searchTime = std::max(0.0f, currentTime_ - 0.05f);
    eventCursor_ = static_cast<std::size_t>(std::lower_bound(
        events_.begin(), events_.end(), searchTime,
        [](const ScheduledEvent& event, float time) {
            return event.time < time;
        }) - events_.begin());

    pendingEvents_.clear();
}

const std::vector<MidiScheduler::ScheduledEvent>&
MidiScheduler::getEventsForWindow(float now, float horizon, float lookback)
{
    pendingEvents_.clear();
    currentTime_ = std::max(0.0f, now);

    if (!playing_ || events_.empty())
        return pendingEvents_;

    const float windowStart = std::max(0.0f, currentTime_ - std::max(0.0f, lookback));
    const float windowEnd = currentTime_ + std::max(0.0f, horizon);

    // A backwards seek can happen without stop() if the UI slider is used.
    if (eventCursor_ > 0 && events_[eventCursor_ - 1].time > windowEnd) {
        eventCursor_ = static_cast<std::size_t>(std::lower_bound(
            events_.begin(), events_.end(), windowStart,
            [](const ScheduledEvent& event, float time) {
                return event.time < time;
            }) - events_.begin());
    }

    // If the cursor is behind the lookback window, skip stale events.
    while (eventCursor_ < events_.size() && events_[eventCursor_].time < windowStart)
        ++eventCursor_;

    // Timestamp future events up to the same 250 ms horizon used by MPWGL2.
    while (eventCursor_ < events_.size() && events_[eventCursor_].time <= windowEnd) {
        pendingEvents_.push_back(events_[eventCursor_]);
        ++eventCursor_;
    }

    return pendingEvents_;
}

} // namespace wasmidi
