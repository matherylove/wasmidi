#include "midi_parser.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace wasmidi {
namespace {

constexpr uint32_t HashSize = 65536;
constexpr uint32_t HashMask = HashSize - 1;
constexpr uint32_t NoIndex = 0xffffffffu;

struct TrackRef {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

struct PendingSlot {
    uint32_t key = 0;
    uint32_t tick = 0;
    uint32_t next = NoIndex;
    uint8_t velocity = 0;
    uint8_t used = 0;
};

struct PendingHash {
    std::array<uint32_t, HashSize> heads{};
    std::vector<PendingSlot> slots;

    void reset(std::size_t reserveCount) {
        heads.fill(NoIndex);
        slots.clear();
        slots.reserve(reserveCount);
    }

    void push(uint32_t key, uint32_t tick, uint8_t velocity) {
        PendingSlot slot;
        slot.key = key;
        slot.tick = tick;
        slot.velocity = velocity;
        slot.used = 1;
        const uint32_t h = key & HashMask;
        slot.next = heads[h];
        heads[h] = static_cast<uint32_t>(slots.size());
        slots.push_back(slot);
    }

    bool pop(uint32_t key, uint32_t& tick, uint8_t& velocity) {
        const uint32_t h = key & HashMask;
        uint32_t* link = &heads[h];
        uint32_t i = *link;

        while (i != NoIndex) {
            PendingSlot& slot = slots[i];
            if (slot.used && slot.key == key) {
                tick = slot.tick;
                velocity = slot.velocity;
                slot.used = 0;
                *link = slot.next;
                return true;
            }
            link = &slot.next;
            i = slot.next;
        }
        return false;
    }
};

inline bool need(const uint8_t* p, const uint8_t* end, std::size_t n) {
    return p <= end && std::size_t(end - p) >= n;
}

uint16_t read16(const uint8_t*& p, const uint8_t* end, bool& ok) {
    if (!ok || !need(p,end,2)) { ok=false; return 0; }
    uint16_t v=(uint16_t(p[0])<<8)|uint16_t(p[1]);
    p+=2; return v;
}

uint32_t read32(const uint8_t*& p, const uint8_t* end, bool& ok) {
    if (!ok || !need(p,end,4)) { ok=false; return 0; }
    uint32_t v=(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|
               (uint32_t(p[2])<<8)|uint32_t(p[3]);
    p+=4; return v;
}

uint32_t varlen(const uint8_t*& p, const uint8_t* end, bool& ok) {
    uint32_t v=0;
    for(int i=0;i<4;i++){
        if(!ok||p>=end){ok=false;return 0;}
        uint8_t b=*p++;
        v=(v<<7)|(b&0x7f);
        if(!(b&0x80)) return v;
    }
    ok=false; return 0;
}

bool statusByte(const uint8_t*& p,const uint8_t* end,uint8_t& running,uint8_t& st){
    if(p>=end) return false;
    st=*p;
    if(st>=0x80){
        ++p;
        if(st<0xf0) running=st;
        else if(st==0xf0||st==0xf7||st==0xff) running=0;
        return true;
    }
    if(!running) return false;
    st=running;
    return true;
}

struct TempoIndex {
    std::vector<double> tick;
    std::vector<double> sec;
    std::vector<double> us;
    uint16_t ppq=480;

    double toSec(uint32_t value) const {
        std::size_t lo=0,hi=tick.size()-1;
        while(lo<hi){
            std::size_t m=(lo+hi+1)>>1;
            if(tick[m]<=double(value)) lo=m; else hi=m-1;
        }
        return sec[lo]+(double(value)-tick[lo])/double(ppq)*(us[lo]/1e6);
    }
};

void buildTempo(const std::vector<TempoChange>& map,uint16_t ppq,TempoIndex& out){
    out.ppq=std::max<uint16_t>(1,ppq);
    double seconds=0.0;
    uint32_t prevTick=0,prevUs=500000;
    for(const auto& t:map){
        seconds += double(t.tick-prevTick)/double(out.ppq)*(double(prevUs)/1e6);
        out.tick.push_back(double(t.tick));
        out.sec.push_back(seconds);
        out.us.push_back(double(t.microsecondsPerBeat?t.microsecondsPerBeat:prevUs));
        prevTick=t.tick;
        if(t.microsecondsPerBeat) prevUs=t.microsecondsPerBeat;
    }
    if(out.tick.empty()){
        out.tick.push_back(0); out.sec.push_back(0); out.us.push_back(500000);
    }
}

uint32_t noteKey(uint16_t track,uint8_t ch,uint8_t note){
    return (uint32_t(track)<<11)|(uint32_t(ch)<<7)|uint32_t(note);
}

} // namespace

const char* MidiParser::error() const { return errorMessage_; }

bool MidiParser::parse(const uint8_t* data,std::size_t size,MidiDocument& output)
{
    output=MidiDocument{};
    errorMessage_="Unknown error";

    if(!data||size<14){errorMessage_="MIDI data is too short";return false;}

    const uint8_t* p=data;
    const uint8_t* end=data+size;
    bool ok=true;

    if(!need(p,end,4)||std::memcmp(p,"MThd",4)!=0){
        errorMessage_="Missing MThd header"; return false;
    }
    p+=4;

    uint32_t headerLen=read32(p,end,ok);
    if(!ok||headerLen<6||!need(p,end,headerLen)){
        errorMessage_="Invalid MIDI header"; return false;
    }

    const uint8_t* headerEnd=p+headerLen;
    output.format=read16(p,headerEnd,ok);
    output.trackCount=read16(p,headerEnd,ok);
    output.ticksPerBeat=read16(p,headerEnd,ok);

    if(!ok||output.format>1){errorMessage_="Only MIDI Format 0 and 1 are supported";return false;}
    if(!output.ticksPerBeat||(output.ticksPerBeat&0x8000)){errorMessage_="SMPTE timing is not supported";return false;}

    p=headerEnd;

    std::vector<TrackRef> tracks;
    tracks.reserve(output.trackCount);

    for(uint16_t i=0;i<output.trackCount;i++){
        if(!need(p,end,8)||std::memcmp(p,"MTrk",4)!=0){
            errorMessage_="Invalid or truncated MTrk chunk";return false;
        }
        p+=4;
        uint32_t len=read32(p,end,ok);
        if(!ok||!need(p,end,len)){errorMessage_="Truncated track data";return false;}
        tracks.push_back({p,len});
        p+=len;
    }

    output.activeChannelMasks.assign(tracks.size(),0);
    output.tempoMap.push_back({0,500000});

    std::size_t noteEstimate=0;
    std::size_t controlEstimate=0;

    // Pass 1: tempo map + exact reserve counts.
    for(std::size_t ti=0;ti<tracks.size();++ti){
        const uint8_t* q=tracks[ti].data;
        const uint8_t* qe=q+tracks[ti].size;
        uint32_t tick=0; uint8_t running=0;

        while(q<qe&&ok){
            tick+=varlen(q,qe,ok);
            uint8_t st=0;
            if(!ok||!statusByte(q,qe,running,st)){ok=false;break;}

            if(st==0xff){
                if(q>=qe){ok=false;break;}
                uint8_t meta=*q++;
                uint32_t len=varlen(q,qe,ok);
                if(!ok||!need(q,qe,len)){ok=false;break;}
                if(meta==0x51&&len>=3){
                    uint32_t us=(uint32_t(q[0])<<16)|(uint32_t(q[1])<<8)|uint32_t(q[2]);
                    output.tempoMap.push_back({tick,us});
                }
                q+=len;
                if(meta==0x2f) break;
                continue;
            }

            if(st==0xf0||st==0xf7){
                uint32_t len=varlen(q,qe,ok);
                if(!ok||!need(q,qe,len)){ok=false;break;}
                q+=len; continue;
            }

            if(st>=0xf0){ok=false;break;}

            uint8_t cmd=st&0xf0, ch=st&0x0f;
            int n=(cmd==0xc0||cmd==0xd0)?1:2;
            if(!need(q,qe,n)){ok=false;break;}
            uint8_t d1=*q++;
            uint8_t d2=n==2?*q++:0;

            if(cmd==0x90){
                output.activeChannelMasks[ti]|=(1u<<ch);
                if(d2>0) ++noteEstimate;
            } else if(cmd==0x80) {
                output.activeChannelMasks[ti]|=(1u<<ch);
            }

            if(cmd==0xb0||cmd==0xc0||cmd==0xd0||cmd==0xe0)
                ++controlEstimate;

            (void)d1;
        }
    }

    if(!ok){errorMessage_="Malformed MIDI event data";output=MidiDocument{};return false;}

    std::stable_sort(output.tempoMap.begin(),output.tempoMap.end(),
        [](const TempoChange&a,const TempoChange&b){return a.tick<b.tick;});

    std::vector<TempoChange> dedup;
    dedup.reserve(output.tempoMap.size());
    for(const auto&t:output.tempoMap){
        if(!dedup.empty()&&dedup.back().tick==t.tick) dedup.back()=t;
        else dedup.push_back(t);
    }
    output.tempoMap.swap(dedup);

    TempoIndex tempo;
    buildTempo(output.tempoMap,output.ticksPerBeat,tempo);

    output.notes.reserve(noteEstimate);
    output.controls.reserve(controlEstimate);

    PendingHash pending;

    for(std::size_t ti=0;ti<tracks.size();++ti){
        const uint8_t* q=tracks[ti].data;
        const uint8_t* qe=q+tracks[ti].size;
        uint32_t tick=0; uint8_t running=0;
        pending.reset(std::min<std::size_t>(noteEstimate,65536));

        while(q<qe&&ok){
            tick+=varlen(q,qe,ok);
            uint8_t st=0;
            if(!ok||!statusByte(q,qe,running,st)){ok=false;break;}

            if(st==0xff){
                if(q>=qe){ok=false;break;}
                uint8_t meta=*q++;
                uint32_t len=varlen(q,qe,ok);
                if(!ok||!need(q,qe,len)){ok=false;break;}
                q+=len;
                if(meta==0x2f) break;
                continue;
            }

            if(st==0xf0||st==0xf7){
                uint32_t len=varlen(q,qe,ok);
                if(!ok||!need(q,qe,len)){ok=false;break;}
                q+=len; continue;
            }

            if(st>=0xf0){ok=false;break;}

            uint8_t cmd=st&0xf0, ch=st&0x0f;
            int n=(cmd==0xc0||cmd==0xd0)?1:2;
            if(!need(q,qe,n)){ok=false;break;}
            uint8_t d1=*q++;
            uint8_t d2=n==2?*q++:0;

            if(cmd==0x90&&d2>0){
                pending.push(noteKey(uint16_t(ti),ch,d1),tick,d2);
                continue;
            }

            if(cmd==0x80||(cmd==0x90&&d2==0)){
                uint32_t onTick=0; uint8_t vel=0;
                if(pending.pop(noteKey(uint16_t(ti),ch,d1),onTick,vel)){
                    float s=float(tempo.toSec(onTick));
                    float e=float(tempo.toSec(tick));
                    if(e<=s)e=s+0.015f;
                    NoteEvent note;
                    note.startTick=onTick; note.endTick=tick>onTick?tick:onTick+1;
                    note.startTime=s; note.endTime=e;
                    note.pitch=d1; note.channel=ch; note.velocity=vel; note.track=uint16_t(ti);
                    output.notes.push_back(note);
                    output.durationSeconds=std::max(output.durationSeconds,e);
                }
                continue;
            }

            if(cmd==0xb0||cmd==0xc0||cmd==0xd0||cmd==0xe0){
                output.controls.push_back({
                    float(tempo.toSec(tick)),cmd,ch,d1,d2
                });
            }
        }

        float trackEnd=float(tempo.toSec(tick));
        for(auto& slot:pending.slots){
            if(!slot.used) continue;
            uint8_t noteNum=uint8_t(slot.key&0x7f);
            uint8_t ch=uint8_t((slot.key>>7)&0x0f);
            float s=float(tempo.toSec(slot.tick));
            float e=trackEnd<=s?s+0.015f:trackEnd;
            NoteEvent note;
            note.startTick=slot.tick; note.endTick=tick>slot.tick?tick:slot.tick+1;
            note.startTime=s; note.endTime=e;
            note.pitch=noteNum; note.channel=ch; note.velocity=slot.velocity; note.track=uint16_t(ti);
            output.notes.push_back(note);
            output.durationSeconds=std::max(output.durationSeconds,e);
        }
    }

    if(!ok){errorMessage_="Malformed MIDI event data";output=MidiDocument{};return false;}

    // In-place sort avoids a second full note buffer on huge Black MIDIs.
    std::sort(output.notes.begin(),output.notes.end(),
        [](const NoteEvent&a,const NoteEvent&b){
            if(a.startTime!=b.startTime)return a.startTime<b.startTime;
            return a.endTime<b.endTime;
        });

    std::sort(output.controls.begin(),output.controls.end(),
        [](const ControlEvent&a,const ControlEvent&b){return a.time<b.time;});

    errorMessage_="";
    return true;
}

double MidiParser::tickToSeconds(uint32_t tick,
    const std::vector<TempoChange>& tempoMap,uint16_t ppq) const
{
    double sec=0.0; uint32_t prevTick=0,us=500000;
    for(const auto&t:tempoMap){
        if(t.tick>tick) break;
        sec += double(t.tick-prevTick)/double(ppq)*(double(us)/1e6);
        prevTick=t.tick;
        if(t.microsecondsPerBeat) us=t.microsecondsPerBeat;
    }
    sec += double(tick-prevTick)/double(ppq)*(double(us)/1e6);
    return sec;
}

} // namespace wasmidi
