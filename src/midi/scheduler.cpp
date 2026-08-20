#include "scheduler.hpp"

#include <algorithm>

namespace wasmidi {

MidiScheduler::MidiScheduler() = default;
MidiScheduler::~MidiScheduler() = default;

void MidiScheduler::setDocument(const MidiDocument* doc) {
    document_ = doc;
    noteCursor_ = 0;
    ccCursor_ = 0;
    pendingEvents_.clear();
}

void MidiScheduler::setSampleRate(float sampleRate) {
    sampleRate_ = sampleRate;
}

void MidiScheduler::start() {
    playing_ = true;
}

void MidiScheduler::stop() {
    playing_ = false;
    currentTime_ = 0.0f;
    noteCursor_ = 0;
    ccCursor_ = 0;
    pendingEvents_.clear();
}

void MidiScheduler::pause() {
    playing_ = false;
}

void MidiScheduler::resume() {
    playing_ = true;
}

void MidiScheduler::seek(float seconds) {
    currentTime_ = seconds;
    
    if (!document_) {
        return;
    }
    
    noteCursor_ = 0;
    ccCursor_ = 0;
    
    for (std::size_t i = 0; i < document_->notes.size(); ++i) {
        if (document_->notes[i].startTime >= seconds) {
            noteCursor_ = i;
            break;
        }
    }
    
    for (std::size_t i = 0; i < document_->controls.size(); ++i) {
        if (document_->controls[i].time >= seconds) {
            ccCursor_ = i;
            break;
        }
    }
    
    pendingEvents_.clear();
}

const std::vector<MidiScheduler::ScheduledEvent>&
MidiScheduler::getEventsForFrame(float horizon) {
    pendingEvents_.clear();
    
    if (!document_ || !playing_) {
        return pendingEvents_;
    }
    
    const float endTime = currentTime_ + horizon;
    
    while (noteCursor_ < document_->notes.size()) {
        const auto& note = document_->notes[noteCursor_];
        
        if (note.startTime > endTime) {
            break;
        }
        
        if (note.startTime >= currentTime_) {
            pendingEvents_.push_back({
                note.startTime,
                0x90,
                note.channel,
                note.pitch,
                note.velocity
            });
        }
        
        if (note.endTime <= endTime && note.endTime > currentTime_) {
            pendingEvents_.push_back({
                note.endTime,
                0x80,
                note.channel,
                note.pitch,
                0
            });
        }
        
        ++noteCursor_;
    }
    
    while (ccCursor_ < document_->controls.size()) {
        const auto& cc = document_->controls[ccCursor_];
        
        if (cc.time > endTime) {
            break;
        }
        
        if (cc.time >= currentTime_) {
            pendingEvents_.push_back({
                cc.time,
                cc.type,
                cc.channel,
                cc.data1,
                cc.data2
            });
        }
        
        ++ccCursor_;
    }
    
    std::sort(
        pendingEvents_.begin(),
        pendingEvents_.end(),
        [](const ScheduledEvent& a, const ScheduledEvent& b) {
            return a.time < b.time;
        }
    );
    
    return pendingEvents_;
}

}