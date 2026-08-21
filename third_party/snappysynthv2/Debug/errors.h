#ifndef ERROR_H
#define ERROR_H

#define ERROR_TITLE "SnappySynth - Uh oh!"

// Init errors
#define RENDERINITFAIL "SnappySynth failed to initialize for render mode"
#define REALTIMEINITFAIL "SnappySynth failed to initialize for realtime mode"

// Audio errors
#define AUDIOAPIINITFAIL "SnappySynth failed to load the audio API.\nPerhaps increase the buffer or choose a different API."
#define BUFFERTOOLOW "The audio API buffer is impossibly small!\nPlease increase it."

// Voice errors
#define VOICEALLOCFAIL "SnappySynth failed to allocate voices!"
#define VOICEMANAGERINITFAIL "SnappySynth failed to initialize voice manager!"

// User Configuration errors
#define CONFIGNOTFOUND "SnappySynth failed to find or automatically create your configuration file.\n Please make sure you have launched the configurator at least once"
#define CONFIGINVALID "Snappysynth failed to load SnappySynth.cfg.\nIs it configured correctly? Any mistakes/Typos?"
#define CONFIGBUFFER0 "SnappySynth failed to load the audio stream:\nBuffer is set to 0, what are you trying to do, hear the future?"
#define CONFIGBUFFERNEGATIVE "SnappySynth failed to load the audio stream:\nBuffer is set to a negative value.\n\nSadly, SnappySynth cannot time travel you stUPID FUCK."
#define CONFIGSAMPLERATE0 "SnappySynth failed to load the audio stream: \nSample Rate is set to 0, even if we loaded, you wouldn't hear jack sHIT."
#define CONFIGSAMPLERATENEGATIVE "SnappySynth failed to load the audio stream: \nSample-Rate is set to negative, bro you tryina listen to some cisuM?"

// Soundfont errors
#define SFZPATHFAIL "SnappySynth could not find your soundfont.\nPlease double check the path for any invalid characters or mistakes."
#define SFZLOADFAIL "SnappySynth could not load your soundfont.\nPerhaps it is unsupported at the moment?"
#define SF2LOADFAIL "SnappySynth could not load your SoundFont 2 file.\nIt may be corrupt or use features we do not support yet."
#define DLSLOADFAIL "SnappySynth could not load your DLS file.\nIt may be corrupt or use features we do not support yet."
#define SFCORRUPT "SnappySynth found a soundfont file, but its internal data looks corrupt or inconsistent."
#define SFUNSUPPORTED "SnappySynth does not support that type of soundfont yet!"

// Soft Errors
#define UNSUPPORTED "SnappySynth does not support that!"

// Fatal Errors
#define OUTOFMEMORY "SnappySynth has run out of memory!\nYou might want to lower the voice count and watch your RAM usage!"

// Impossible for user errors
#define HOWTHEFUCK "How the fuck did you get here?"
#define MISSINGSYSTEMFOLDERWINDOWS "SnappySynth has failed to load Windows- wait WHAT THE FUCK?"
#define BADINPUT "Why."

#endif // ERROR_H
