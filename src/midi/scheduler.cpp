#include "scheduler.hpp"

#include <algorithm>

namespace wasmidi {

MidiScheduler::MidiScheduler() = default;
MidiScheduler::~MidiScheduler() = default;

void MidiScheduler::setDocument(const MidiDocument* doc)
{
    document_ = doc;
    currentTime_ = 0.0f;
    noteCursor_ = 0;
    controlCursor_ = 0;
    soundingNotes_.clear();
    pendingEvents_.clear();
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
    // MPWGL2 performs AllOff and clears soundingNotes when pausing.
    playing_ = false;
    soundingNotes_.clear();
    pendingEvents_.clear();
}

void MidiScheduler::resume()
{
    playing_ = true;
}

void MidiScheduler::stop()
{
    playing_ = false;
    currentTime_ = 0.0f;
    noteCursor_ = 0;
    controlCursor_ = 0;
    soundingNotes_.clear();
    pendingEvents_.clear();
}

void MidiScheduler::seek(float seconds)
{
    currentTime_ = std::max(0.0f, seconds);
    soundingNotes_.clear();
    pendingEvents_.clear();

    if (!document_) {
        noteCursor_ = 0;
        controlCursor_ = 0;
        return;
    }

    // resetDispatch()/resume parity: rewind 50 ms.
    const float from = std::max(0.0f, currentTime_ - 0.05f);

    noteCursor_ = static_cast<std::size_t>(
        std::lower_bound(
            document_->notes.begin(),
            document_->notes.end(),
            from,
            [](const NoteEvent& note, float time) {
                return note.startTime < time;
            }) - document_->notes.begin());

    controlCursor_ = static_cast<std::size_t>(
        std::lower_bound(
            document_->controls.begin(),
            document_->controls.end(),
            from,
            [](const ControlEvent& event, float time) {
                return event.time < time;
            }) - document_->controls.begin());
}

const std::vector<MidiScheduler::ScheduledEvent>&
MidiScheduler::getEventsForWindow(
    float now,
    float horizon,
    float lookback)
{
    pendingEvents_.clear();
    currentTime_ = std::max(0.0f, now);

    if (!playing_ || !document_)
        return pendingEvents_;

    const float horizonTime =
        currentTime_ + std::max(0.0f, horizon);
    const float earliest =
        currentTime_ - std::max(0.0f, lookback);

    // MPWGL2: while(dispCursor < noteCount &&
    //               startArr[dispCursor] < horizon)
    while (noteCursor_ < document_->notes.size() &&
           document_->notes[noteCursor_].startTime < horizonTime) {
        const NoteEvent& note = document_->notes[noteCursor_];

        if (note.startTime >= earliest) {
            pendingEvents_.push_back({
                note.startTime,
                0x90,
                note.channel,
                note.pitch,
                note.velocity,
                note.track
            });

            // Same key used by the legacy Map: channel*128+pitch.
            const uint16_t key =
                static_cast<uint16_t>(
                    uint16_t(note.channel) * 128u +
                    uint16_t(note.pitch));

            soundingNotes_[key] = {
                note.endTime,
                note.channel,
                note.pitch,
                note.track
            };
        }

        ++noteCursor_;
    }

    // MPWGL2 control cursor uses the same 250 ms horizon.
    while (controlCursor_ < document_->controls.size()) {
        const ControlEvent& event =
            document_->controls[controlCursor_];

        if (event.time > horizonTime)
            break;

        if (event.time >= earliest) {
            pendingEvents_.push_back({
                event.time,
                event.type,
                event.channel,
                event.data1,
                event.data2,
                0
            });
        }

        ++controlCursor_;
    }

    // MPWGL2 only schedules note-off when the end is effectively due,
    // instead of pre-queuing every NoteOff 250 ms in advance.
    for (auto it = soundingNotes_.begin();
         it != soundingNotes_.end();) {
        const SoundingNote& note = it->second;

        if (note.endSec <= currentTime_ + 0.008f ||
            note.endSec < currentTime_ - 0.5f) {
            pendingEvents_.push_back({
                std::max(note.endSec, currentTime_),
                0x80,
                note.channel,
                note.note,
                0,
                note.track
            });

            it = soundingNotes_.erase(it);
        } else {
            ++it;
        }
    }

    return pendingEvents_;
}

} // namespace wasmidi
