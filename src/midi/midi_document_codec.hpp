#pragma once

#include "midi_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wasmidi {

// Stable compact wire format used only between the dedicated browser MIDI
// parser Worker and the Qt WASM instance. It intentionally mirrors the final
// MidiDocument arrays so the UI thread does not repeat MIDI parsing work.
bool serializeMidiDocument(
    const MidiDocument& document,
    std::vector<uint8_t>& bytes,
    std::string& error);

bool deserializeMidiDocument(
    const uint8_t* bytes,
    std::size_t size,
    MidiDocument& document,
    std::string& error);

} // namespace wasmidi
