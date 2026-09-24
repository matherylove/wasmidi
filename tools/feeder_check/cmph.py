import sys, hashlib
def h(p, limit):
    b=open(p,'rb').read(); i=0; hm=hashlib.sha1(); hs=hashlib.sha1(); n=0
    while i<len(b) and n<limit:
        ty=b[i]; v=int.from_bytes(b[i+9:i+13],'little')
        if ty==2: hs.update(b[i:i+13+v]); i+=13+v
        else: hm.update(b[i+1:i+13]); i+=13; n+=1
    return hm.hexdigest(), hs.hexdigest(), n
def count(p):
    b=open(p,'rb').read(); i=0; n=0
    while i<len(b):
        ty=b[i]; v=int.from_bytes(b[i+9:i+13],'little'); i+=13+(v if ty==2 else 0); n+=(ty==1)
    return n
lim=min(count(sys.argv[1]),count(sys.argv[2]))
a=h(sys.argv[1],lim); b=h(sys.argv[2],lim)
print('compared msgs',lim,'EQUAL' if a==b else 'DIFF')
