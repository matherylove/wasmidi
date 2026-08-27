#include "scheduler.hpp"

#include <algorithm>
#include <cmath>

namespace wasmidi {

MidiScheduler::MidiScheduler()
{
    pendingEvents_.reserve(4096);
}

MidiScheduler::~MidiScheduler() = default;

void MidiScheduler::setDocument(const MidiDocument* doc)
{
    document_ = doc;
    currentTime_ = 0.0f;
    groupCursor_ = 0;
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
    playing_ = false;
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
    groupCursor_ = 0;
    pendingEvents_.clear();
}

void MidiScheduler::seek(float seconds)
{
    currentTime_ = std::max(0.0f, seconds);
    pendingEvents_.clear();

    if (!document_) {
        groupCursor_ = 0;
        return;
    }

    // Same small resume/seek rewind used by the previous MPWGL2 port, but the
    // cursor now points into the sparse tick index instead of a NoteEvent list.
    const double fromSeconds =
        std::max(0.0, double(currentTime_) - 0.05);

    const uint32_t fromTick =
        static_cast<uint32_t>(
            std::max(
                0.0,
                std::floor(
                    document_->secondsToTick(
                        fromSeconds))));

    groupCursor_ =
        document_->lowerBoundGroup(fromTick);
}

const std::vector<MidiScheduler::ScheduledEvent>&
MidiScheduler::getEventsForWindow(
    float now,
    float horizon,
    float lookback)
{
    pendingEvents_.clear();
    currentTime_ = std::max(0.0f, now);

    if (!playing_ ||
        !document_ ||
        document_->tickGroups.empty()) {
        return pendingEvents_;
    }

    const double horizonSeconds =
        double(currentTime_) +
        std::max(0.0f, horizon);

    const double earliestSeconds =
        std::max(
            0.0,
            double(currentTime_) -
            std::max(0.0f, lookback));

    const uint32_t horizonTick =
        static_cast<uint32_t>(
            std::min<double>(
                document_->maxTick,
                std::ceil(
                    document_->secondsToTick(
                        horizonSeconds))));

    while (groupCursor_ <
           document_->tickGroups.size()) {
        const TickGroup& group =
            document_->tickGroups[groupCursor_];

        if (group.tick > horizonTick)
            break;

        const double groupSeconds =
            document_->tickToSeconds(
                group.tick);

        if (groupSeconds >= earliestSeconds) {
            const std::size_t begin =
                group.eventOffset;

            const std::size_t end =
                begin + group.eventCount;

            pendingEvents_.reserve(
                pendingEvents_.size() +
                group.eventCount);

            for (std::size_t i = begin;
                 i < end;
                 ++i) {
                const CompactEvent& event =
                    document_->events[i];

                // Channel-event stream is already normalized:
                // NoteOn velocity zero was converted to NoteOff by the parser.
                pendingEvents_.push_back({
                    static_cast<float>(
                        groupSeconds),
                    static_cast<uint8_t>(
                        event.status & 0xf0),
                    static_cast<uint8_t>(
                        event.status & 0x0f),
                    event.data1,
                    event.data2,
                    0
                });
            }
        }

        ++groupCursor_;
    }

    return pendingEvents_;
}

} // namespace wasmidi
