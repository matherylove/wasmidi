"use strict";

// Background render-page builder for the horizontal visualizer and keyboard.
//
// The cache is deliberately page/state based instead of storing 64 full RGBA
// screenshots. A full-HD 64-page RGBA cache would consume ~506 MiB before
// browser/driver copies. Each horizontal page stores only the notes that can
// still contribute to the final raster, while each keyboard page is an exact
// compact state snapshot. Both are prepared away from Qt's UI/render thread.
//
// Pages are one screen-sized interval in MIDI-tick space. The worker scores all
// requested pages for density/crashpoint difficulty and emits the current page
// first, then the hardest pages, so expensive regions are already prepared when
// playback reaches them. Missing pages always have a C++ raw-index recovery
// path; cache completion is never required for correctness.

let generation = 0;
let notes = null; // Uint32Array: [startTick,endTick,packedData]...
let noteCount = 0;
let maxTick = 0;
let primeToken = 0;
let difficultySpanTicks = 0;
let difficultyCache = new Map();

let keyStarts = null; // [tick,count,packedData]...
let keyEnds = null;   // [tick,count,packedData]...
let keyOwners = null; // [tick,packedData]...
let keyStartCount = 0;
let keyEndCount = 0;
let keyOwnerCount = 0;
let keyCheckpoints = [];

const KEY_PITCHES = 128;
const KEY_COLORS = 16;
const KEY_COUNT_WORDS = KEY_PITCHES * KEY_COLORS;
const KEY_HEADER_WORDS = 3;
const KEY_OWNER_WORDS = KEY_PITCHES;
const KEY_PAGE_WORDS =
    KEY_HEADER_WORDS + KEY_COUNT_WORDS * 2 + KEY_OWNER_WORDS;

function lowerBoundStart(tick) {
    let lo = 0;
    let hi = noteCount;
    while (lo < hi) {
        const mid = (lo + hi) >>> 1;
        if (notes[mid * 3] < tick)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

function pageBounds(pageIndex, spanTicks) {
    const start = Math.min(maxTick, pageIndex * spanTicks);
    const end = Math.min(maxTick + 1, start + spanTicks);
    return { start, end };
}

function scorePage(pageIndex, spanTicks) {
    if (difficultySpanTicks !== spanTicks) {
        difficultySpanTicks = spanTicks;
        difficultyCache.clear();
    }

    const cached = difficultyCache.get(pageIndex);
    if (cached)
        return cached;

    const bounds = pageBounds(pageIndex, spanTicks);
    const begin = lowerBoundStart(bounds.start);
    const end = lowerBoundStart(bounds.end);
    const count = Math.max(0, end - begin);
    if (!count) {
        const empty = { pageIndex, score: 0, begin, end };
        difficultyCache.set(pageIndex, empty);
        return empty;
    }

    // Crashpoint heuristic: raw NoteOn density + long-note pressure + pitch
    // concentration. This is intentionally cheap and deterministic.
    const perPitch = new Uint32Array(128);
    let durationPressure = 0;
    let peakPitch = 0;
    let peakSameTick = 0;
    let sameTickRun = 0;
    let previousStart = 0xffffffff;

    for (let i = begin; i < end; ++i) {
        const base = i * 3;
        const startTick = notes[base];
        const endTick = notes[base + 1];
        const packed = notes[base + 2];
        const pitch = (packed >>> 8) & 0x7f;
        const pitchCount = ++perPitch[pitch];
        if (pitchCount > peakPitch)
            peakPitch = pitchCount;

        if (startTick === previousStart) {
            ++sameTickRun;
        } else {
            previousStart = startTick;
            sameTickRun = 1;
        }
        if (sameTickRun > peakSameTick)
            peakSameTick = sameTickRun;

        const duration = Math.max(1, endTick - startTick);
        durationPressure += Math.min(8, duration / Math.max(1, spanTicks));
    }

    const score =
        count +
        durationPressure * 0.35 +
        peakPitch * 0.75 +
        peakSameTick * 1.50;

    const result = { pageIndex, score, begin, end };
    difficultyCache.set(pageIndex, result);
    return result;
}

function buildPage(job) {
    const begin = job.begin;
    const end = job.end;
    const sourceCount = Math.max(0, end - begin);

    if (!sourceCount) {
        return {
            pageIndex: job.pageIndex,
            sourceCount: 0,
            score: job.score,
            words: new Uint32Array(0)
        };
    }

    // One byte per source note is much cheaper than accumulating millions of
    // JavaScript Number objects for pathological same-tick Black MIDIs.
    const keep = new Uint8Array(sourceCount);
    let keptCount = 0;
    let groupBegin = begin;

    while (groupBegin < end) {
        const startTick = notes[groupBegin * 3];
        let groupEnd = groupBegin + 1;
        while (groupEnd < end && notes[groupEnd * 3] === startTick)
            ++groupEnd;

        // Exact same-start/same-pitch occlusion can be removed before any
        // viewport is known. Later source entries are opaque and preserve the
        // MPWGL2 overwrite rule, so an earlier interval fully contained by a
        // later interval can never contribute a pixel.
        const maxLaterEnd = new Uint32Array(128);
        const seenPitch = new Uint8Array(128);

        for (let i = groupEnd - 1; i >= groupBegin; --i) {
            const base = i * 3;
            const endTick = notes[base + 1];
            const pitch = (notes[base + 2] >>> 8) & 0x7f;

            if (seenPitch[pitch] && maxLaterEnd[pitch] >= endTick)
                continue;

            seenPitch[pitch] = 1;
            if (endTick > maxLaterEnd[pitch])
                maxLaterEnd[pitch] = endTick;
            keep[i - begin] = 1;
            ++keptCount;
        }

        groupBegin = groupEnd;
    }

    const output = new Uint32Array(keptCount * 3);
    let out = 0;
    for (let i = begin; i < end; ++i) {
        if (!keep[i - begin])
            continue;
        const base = i * 3;
        output[out++] = notes[base];
        output[out++] = notes[base + 1];
        output[out++] = notes[base + 2];
    }

    return {
        pageIndex: job.pageIndex,
        sourceCount,
        score: job.score,
        words: output
    };
}

function createKeyState() {
    const globalCounts = new Uint32Array(KEY_COUNT_WORDS);
    const trackCounts = new Uint32Array(KEY_COUNT_WORDS);
    const pitchCounts = new Uint32Array(KEY_PITCHES);
    const ownerGlobal = new Uint8Array(KEY_PITCHES);
    const ownerTrack = new Uint8Array(KEY_PITCHES);
    ownerGlobal.fill(0xff);
    ownerTrack.fill(0xff);
    return {
        tick: 0,
        startCursor: 0,
        endCursor: 0,
        ownerCursor: 0,
        globalCounts,
        trackCounts,
        pitchCounts,
        ownerGlobal,
        ownerTrack
    };
}

function cloneKeyState(state) {
    return {
        tick: state.tick,
        startCursor: state.startCursor,
        endCursor: state.endCursor,
        ownerCursor: state.ownerCursor,
        globalCounts: state.globalCounts.slice(),
        trackCounts: state.trackCounts.slice(),
        pitchCounts: state.pitchCounts.slice(),
        ownerGlobal: state.ownerGlobal.slice(),
        ownerTrack: state.ownerTrack.slice()
    };
}

function saturatingAdd(value, amount) {
    return Math.min(0xffffffff, value + amount) >>> 0;
}

function fallbackOwner(counts, pitch) {
    const row = pitch * KEY_COLORS;
    for (let color = KEY_COLORS - 1; color >= 0; --color) {
        if (counts[row + color] !== 0)
            return color;
    }
    return 0xff;
}

function applyKeyStart(state, packed, count) {
    const pitch = packed & 0x7f;
    const globalColor = (packed >>> 8) & 0x0f;
    const trackColor = (packed >>> 12) & 0x0f;
    const row = pitch * KEY_COLORS;

    state.pitchCounts[pitch] =
        saturatingAdd(state.pitchCounts[pitch], count);
    state.globalCounts[row + globalColor] =
        saturatingAdd(state.globalCounts[row + globalColor], count);
    state.trackCounts[row + trackColor] =
        saturatingAdd(state.trackCounts[row + trackColor], count);
}

function applyKeyOwner(state, packed) {
    const pitch = packed & 0x7f;
    if (!state.pitchCounts[pitch])
        return;
    state.ownerGlobal[pitch] = (packed >>> 8) & 0x0f;
    state.ownerTrack[pitch] = (packed >>> 12) & 0x0f;
}

function applyKeyEnd(state, packed, count) {
    const pitch = packed & 0x7f;
    const globalColor = (packed >>> 8) & 0x0f;
    const trackColor = (packed >>> 12) & 0x0f;
    const row = pitch * KEY_COLORS;

    const globalIndex = row + globalColor;
    const trackIndex = row + trackColor;
    const removed = Math.min(
        count,
        state.globalCounts[globalIndex],
        state.trackCounts[trackIndex],
        state.pitchCounts[pitch]);

    state.globalCounts[globalIndex] -= removed;
    state.trackCounts[trackIndex] -= removed;
    state.pitchCounts[pitch] -= removed;

    if (!state.pitchCounts[pitch]) {
        state.ownerGlobal[pitch] = 0xff;
        state.ownerTrack[pitch] = 0xff;
        return;
    }

    if (state.ownerGlobal[pitch] === globalColor &&
        state.globalCounts[globalIndex] === 0) {
        state.ownerGlobal[pitch] =
            fallbackOwner(state.globalCounts, pitch);
    }

    if (state.ownerTrack[pitch] === trackColor &&
        state.trackCounts[trackIndex] === 0) {
        state.ownerTrack[pitch] =
            fallbackOwner(state.trackCounts, pitch);
    }
}

function advanceKeyState(state, targetTick) {
    while (state.startCursor < keyStartCount &&
           keyStarts[state.startCursor * 3] <= targetTick) {
        const base = state.startCursor * 3;
        applyKeyStart(
            state,
            keyStarts[base + 2],
            keyStarts[base + 1]);
        ++state.startCursor;
    }

    while (state.ownerCursor < keyOwnerCount &&
           keyOwners[state.ownerCursor * 2] <= targetTick) {
        const base = state.ownerCursor * 2;
        applyKeyOwner(state, keyOwners[base + 1]);
        ++state.ownerCursor;
    }

    // Inclusive note lifetime: an end exactly at targetTick remains active.
    while (state.endCursor < keyEndCount &&
           keyEnds[state.endCursor * 3] < targetTick) {
        const base = state.endCursor * 3;
        applyKeyEnd(
            state,
            keyEnds[base + 2],
            keyEnds[base + 1]);
        ++state.endCursor;
    }

    state.tick = targetTick;
}

function buildKeyCheckpoints() {
    keyCheckpoints = [];
    if (!keyStarts || !keyEnds || !keyOwners)
        return;

    const state = createKeyState();
    // 128 snapshots cost ~2.1 MiB and bound arbitrary seek replay to roughly
    // 1/128 of the song before a rolling page snapshot becomes available.
    const checkpointCount = 128;
    const span = Math.max(1, Math.ceil((maxTick + 1) / checkpointCount));

    for (let i = 0; i <= checkpointCount; ++i) {
        const tick = Math.min(maxTick, i * span);
        advanceKeyState(state, tick);
        keyCheckpoints.push(cloneKeyState(state));
        if (tick >= maxTick)
            break;
    }
}

function checkpointForTick(tick) {
    if (!keyCheckpoints.length)
        return createKeyState();

    let lo = 0;
    let hi = keyCheckpoints.length;
    while (lo < hi) {
        const mid = (lo + hi) >>> 1;
        if (keyCheckpoints[mid].tick <= tick)
            lo = mid + 1;
        else
            hi = mid;
    }

    const index = Math.max(0, lo - 1);
    return cloneKeyState(keyCheckpoints[index]);
}

function buildKeyboardPage(pageIndex, spanTicks) {
    const tick = Math.min(maxTick, pageIndex * spanTicks);
    const state = checkpointForTick(tick);
    if (state.tick < tick)
        advanceKeyState(state, tick);

    const words = new Uint32Array(KEY_PAGE_WORDS);
    words[0] = state.startCursor >>> 0;
    words[1] = state.endCursor >>> 0;
    words[2] = state.ownerCursor >>> 0;

    let offset = KEY_HEADER_WORDS;
    words.set(state.globalCounts, offset);
    offset += KEY_COUNT_WORDS;
    words.set(state.trackCounts, offset);
    offset += KEY_COUNT_WORDS;

    for (let pitch = 0; pitch < KEY_PITCHES; ++pitch) {
        words[offset++] =
            (state.ownerGlobal[pitch] & 0xff) |
            ((state.ownerTrack[pitch] & 0xff) << 8);
    }

    return words;
}

function postPage(localGeneration, spanTicks, page) {
    const buffer = page.words.buffer;
    postMessage({
        type: "page",
        generation: localGeneration,
        spanTicks,
        pageIndex: page.pageIndex,
        sourceCount: page.sourceCount,
        difficulty: page.score,
        data: buffer
    }, [buffer]);
}

function postKeyboardPage(localGeneration, spanTicks, pageIndex, words) {
    const buffer = words.buffer;
    postMessage({
        type: "keyPage",
        generation: localGeneration,
        spanTicks,
        pageIndex,
        data: buffer
    }, [buffer]);
}

async function buildPrime(message) {
    if (!notes || message.generation !== generation)
        return;

    const localToken = ++primeToken;
    const localGeneration = generation;
    const spanTicks = Math.max(1, Number(message.spanTicks) >>> 0);
    const firstPage = Math.max(0, Number(message.firstPage) >>> 0);
    const count = Math.max(1, Math.min(64, Number(message.count) | 0));
    const currentPage = Math.max(firstPage, Number(message.currentPage) >>> 0);

    const jobs = [];
    for (let i = 0; i < count; ++i) {
        const pageIndex = firstPage + i;
        const bounds = pageBounds(pageIndex, spanTicks);
        if (bounds.start > maxTick)
            break;
        jobs.push(scorePage(pageIndex, spanTicks));
    }

    // Recovery page first. After that, intentionally render the difficult
    // crashpoints before cheap empty/sparse pages; distance is only a tie-break.
    jobs.sort((a, b) => {
        if (a.pageIndex === currentPage) return -1;
        if (b.pageIndex === currentPage) return 1;
        if (b.score !== a.score) return b.score - a.score;
        return Math.abs(a.pageIndex - currentPage) -
               Math.abs(b.pageIndex - currentPage);
    });

    for (let i = 0; i < jobs.length; ++i) {
        if (localToken !== primeToken || localGeneration !== generation)
            return;

        const job = jobs[i];
        const page = buildPage(job);
        postPage(localGeneration, spanTicks, page);

        // The keyboard snapshot is emitted in the same difficulty order. Its
        // payload is tiny (~16.5 KiB) and lets a seek/catch-up restore key state
        // without scanning the dense visual note range on Qt's UI thread.
        if (keyStarts && keyEnds && keyOwners) {
            const keyPage = buildKeyboardPage(job.pageIndex, spanTicks);
            postKeyboardPage(
                localGeneration,
                spanTicks,
                job.pageIndex,
                keyPage);
        }

        // Let seek/config messages pre-empt a long 64-page fill.
        if ((i & 1) === 1)
            await new Promise(resolve => setTimeout(resolve, 0));
    }
}

self.onmessage = event => {
    const message = event.data || {};

    if (message.type === "install") {
        ++primeToken;
        generation = Number(message.generation) >>> 0;
        maxTick = Number(message.maxTick) >>> 0;
        notes = new Uint32Array(message.notes || new ArrayBuffer(0));
        noteCount = Math.floor(notes.length / 3);

        keyStarts = new Uint32Array(message.keyStarts || new ArrayBuffer(0));
        keyEnds = new Uint32Array(message.keyEnds || new ArrayBuffer(0));
        keyOwners = new Uint32Array(message.keyOwners || new ArrayBuffer(0));
        keyStartCount = Math.floor(keyStarts.length / 3);
        keyEndCount = Math.floor(keyEnds.length / 3);
        keyOwnerCount = Math.floor(keyOwners.length / 2);
        difficultySpanTicks = 0;
        difficultyCache.clear();
        buildKeyCheckpoints();

        postMessage({
            type: "installed",
            generation,
            noteCount,
            keyStartCount,
            keyEndCount
        });
        return;
    }

    if (message.type === "prime") {
        buildPrime(message).catch(error => {
            postMessage({
                type: "error",
                generation,
                message: error && error.message
                    ? error.message
                    : String(error || "visual cache worker failed")
            });
        });
        return;
    }

    if (message.type === "clear") {
        ++primeToken;
        notes = null;
        noteCount = 0;
        maxTick = 0;
        keyStarts = null;
        keyEnds = null;
        keyOwners = null;
        keyStartCount = 0;
        keyEndCount = 0;
        keyOwnerCount = 0;
        keyCheckpoints = [];
        difficultySpanTicks = 0;
        difficultyCache.clear();
    }
};
