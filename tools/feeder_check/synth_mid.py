import random, struct
random.seed(7)
def vlq(n):
    b=[n&127]; n>>=7
    while n: b.append((n&127)|128); n>>=7
    return bytes(reversed(b))
def track(events):
    # events: list of (tick, bytes) already ordered
    out=b''; last=0
    for t,e in events: out+=vlq(t-last)+e; last=t
    out+=vlq(0)+b'\xff\x2f\x00'
    return b'MTrk'+struct.pack('>I',len(out))+out
tracks=[]
# track 0: tempo changes incl. duplicates at same tick, sysex resets, GM on
ev=[(0,b'\xff\x51\x03\x07\xa1\x20'),(0,b'\xf0\x05\x7e\x7f\x09\x01\xf7'),(480,b'\xff\x51\x03\x05\x00\x00'),(960,b'\xff\x51\x03\x0f\x42\x40'),(960,b'\xff\x51\x03\x03\x00\x00'),
    (3000,b'\xf0\x0a\x41\x10\x42\x12\x40\x00\x7f\x00\x41\xf7'),(3000,b'\xff\x01\x03abc'),(5000,b'\xf0\x08\x43\x10\x4c\x00\x00\x7e\x00\xf7'),(7000,b'\xf0\x03\x01\x02\xf7'),(7000,b'\xf7\x02\x05\x06')]
tracks.append(track(ev))
for tr in range(1,6):
    ev=[]; t=0; run=None
    for i in range(3000):
        t+=random.choice([0,0,0,1,2,5,10])
        ch=random.randrange(16)
        r=random.random()
        if r<0.45: st=0x90|ch; data=bytes([random.randrange(128), random.choice([0,1,20,64,100,127])])
        elif r<0.7: st=0x80|ch; data=bytes([random.randrange(128), random.randrange(128)])
        elif r<0.8: st=0xb0|ch; data=bytes([random.choice([0,32,7,10,0,32]), random.randrange(128)])
        elif r<0.87: st=0xc0|ch; data=bytes([random.randrange(128)])
        elif r<0.94: st=0xe0|ch; data=bytes([random.randrange(128), random.randrange(128)])
        elif r<0.96: st=0xa0|ch; data=bytes([random.randrange(128), random.randrange(128)])
        elif r<0.98: st=0xd0|ch; data=bytes([random.randrange(128)])
        else:
            ev.append((t,b'\xff\x06\x02hi')); run=None; continue
        if st==run and random.random()<0.6: ev.append((t,data))
        else: ev.append((t,bytes([st])+data)); run=st
        if random.random()<0.05:  # duplicate identical note-on stack
            n=bytes([0x90|ch, 60, 90]); ev.append((t,n)); ev.append((t,n)); run=0x90|ch
    if tr==3: ev.append((6000,b'\xff\x51\x03\x02\x00\x00'))
    ev.sort(key=lambda x:x[0])
    tracks.append(track(ev))
hdr=b'MThd'+struct.pack('>IHHH',6,1,len(tracks),96)
open(__import__('sys').argv[1] if len(__import__('sys').argv)>1 else 'synth.mid','wb').write(hdr+b''.join(tracks))
print('ok')
