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

void MidiScheduler::setSampleRate(float sampleRate) { sampleRate_ = sampleRate; }
void MidiScheduler::start() { playing_ = true; }
void MidiScheduler::pause() { playing_ = false; }
void MidiScheduler::resume() { playing_ = true; }

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
        events_.push_back({note.startTime, 0x90, note.channel, note.pitch, note.velocity, note.track});
        events_.push_back({note.endTime, 0x80, note.channel, note.pitch, 0, note.track});
    }
    for (const auto& event : document_->controls)
        events_.push_back({event.time, event.type, event.channel, event.data1, event.data2, 0});

    std::stable_sort(events_.begin(), events_.end(), [](const ScheduledEvent& a, const ScheduledEvent& b) {
        if (a.time != b.time)
            return a.time < b.time;
        // Release notes before retriggering the same timestamp.
        if (a.type == 0x80 && b.type == 0x90) return true;
        if (a.type == 0x90 && b.type == 0x80) return false;
        return false;
    });
}

void MidiScheduler::seek(float seconds)
{
    currentTime_ = std::max(0.0f, seconds);
    eventCursor_ = static_cast<std::size_t>(std::lower_bound(
        events_.begin(), events_.end(), currentTime_,
        [](const ScheduledEvent& event, float time) { return event.time < time; }) - events_.begin());
    pendingEvents_.clear();
}

const std::vector<MidiScheduler::ScheduledEvent>& MidiScheduler::getEventsForFrame(float horizon)
{
    pendingEvents_.clear();
    if (!playing_ || events_.empty())
        return pendingEvents_;

    const float endTime = currentTime_ + std::max(0.0f, horizon);
    while (eventCursor_ < events_.size() && events_[eventCursor_].time <= endTime) {
        if (events_[eventCursor_].time >= currentTime_)
            pendingEvents_.push_back(events_[eventCursor_]);
        ++eventCursor_;
    }
    currentTime_ = endTime;
    return pendingEvents_;
}

} // namespace wasmidi
