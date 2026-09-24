# Minimal SF2: 11 looped samples at 44.1 kHz, one preset (bank 0 prog 0) with 11 key zones.
import struct, math, random
SR=44100; N=11
def chunk(cid, data):
    if len(data)%2: data+=b'\0'
    return cid+struct.pack('<I',len(data))+data
def lst(t, body): return chunk(b'LIST', t+body)
random.seed(1)
samples=[]; shdr=b''; smpl=b''; start=0
for i in range(N):
    n=SR//2
    root=24+i*9
    f=440*2**((root-69)/12)
    per=SR/f
    cyc=max(1,int(n/per)); n=int(cyc*per)
    data=[int(12000*math.sin(2*math.pi*k/per)+random.randint(-300,300)) for k in range(n)]
    smpl+=struct.pack('<%dh'%n,*data)+b'\0\0'*46
    name=('s%d'%i).encode().ljust(20,b'\0')
    shdr+=name+struct.pack('<IIIIIBbHH',start,start+n,start+n//4,start+n-8,SR,root,0,0,1)
    start+=n+46
shdr+=b'EOS'.ljust(20,b'\0')+struct.pack('<IIIIIBbHH',0,0,0,0,0,0,0,0,0)
def gen(op,val): return struct.pack('<Hh',op,val)
def genr(op,lo,hi): return struct.pack('<HBB',op,lo,hi)
igen=b''; ibag=b''; gi=0
for i in range(N):
    lo=i*12; hi=min(127,lo+11) if i<N-1 else 127
    ibag+=struct.pack('<HH',gi,0)
    g=genr(43,lo,hi)+gen(38,-1200)+gen(36,-3986)+gen(54,1)+gen(53,i)  # keyRange, releaseVol 0.5s, decay, loop, sampleID
    igen+=g; gi+=5
ibag+=struct.pack('<HH',gi,0); igen+=struct.pack('<HH',0,0)
inst=b'inst0'.ljust(20,b'\0')+struct.pack('<H',0)+b'EOI'.ljust(20,b'\0')+struct.pack('<H',N)
phdr=b'preset0'.ljust(20,b'\0')+struct.pack('<HHHIII',0,0,0,0,0,0)+b'EOP'.ljust(20,b'\0')+struct.pack('<HHHIII',0,0,1,0,0,0)
pbag=struct.pack('<HH',0,0)+struct.pack('<HH',1,0)
pgen=gen(41,0)+struct.pack('<HH',0,0)
pmod=b'\0'*10; imod=b'\0'*10
info=lst(b'INFO',chunk(b'ifil',struct.pack('<HH',2,1))+chunk(b'isng',b'EMU8000\0')+chunk(b'INAM',b'bench\0'))
sdta=lst(b'sdta',chunk(b'smpl',smpl))
pdta=lst(b'pdta',chunk(b'phdr',phdr)+chunk(b'pbag',pbag)+chunk(b'pmod',pmod)+chunk(b'pgen',pgen)+chunk(b'inst',inst)+chunk(b'ibag',ibag)+chunk(b'imod',imod)+chunk(b'igen',igen)+chunk(b'shdr',shdr))
open('bench.sf2','wb').write(chunk(b'RIFF',b'sfbk'+info+sdta+pdta))
print("ok")
