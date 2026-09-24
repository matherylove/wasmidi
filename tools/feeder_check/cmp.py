import sys
def recs(p):
    b=open(p,'rb').read(); i=0; m=[]; s=[]
    while i<len(b):
        ty=b[i]; t=b[i+1:i+9]; v=int.from_bytes(b[i+9:i+13],'little'); i+=13
        if ty==2: s.append((t,b[i:i+v])); i+=v
        else: m.append((t,v))
    return m,s
am,asx=recs(sys.argv[1]); bm,bsx=recs(sys.argv[2])
n=min(len(am),len(bm))
print('msgs',len(am),len(bm),'equal prefix' if am[:n]==bm[:n] else 'DIFF', '| sysex',len(asx),len(bsx),'equal' if asx==bsx else 'DIFF')
