import socket, time, sys, os
from PIL import Image
D=os.path.dirname(os.path.abspath(__file__))
class Q:
    def __init__(s):
        s.s=socket.socket(socket.AF_UNIX); s.s.connect(D+'/mon.sock'); time.sleep(.3); s.s.settimeout(.5); s.rd()
        s.x=s.y=0
    def rd(s):
        try: return s.s.recv(65536).decode(errors='ignore')
        except Exception: return ''
    def cmd(s,c,w=0.08):
        s.s.send((c+'\n').encode()); time.sleep(w); return s.rd()
    def key(s,k,w=.15): s.cmd('sendkey '+k,w)
    def type(s,t):
        for ch in t:
            s.key({' ':'spc','.':'dot','-':'minus','/':'slash',':':'shift-semicolon','?':'shift-slash','=':'equal','_':'shift-minus'}.get(ch,ch if not ch.isupper() else 'shift-'+ch.lower()),.08)
    def shot(s,name):
        s.cmd('screendump %s/%s.ppm'%(D,name),0.6)
        Image.open('%s/%s.ppm'%(D,name)).save('%s/%s.png'%(D,name)); os.remove('%s/%s.ppm'%(D,name))
        return '%s/%s.png'%(D,name)
    def move(s,dx,dy):
        while dx or dy:
            sx=max(-120,min(120,dx)); sy=max(-120,min(120,dy))
            s.cmd('mouse_move %d %d'%(sx,sy),0.02); dx-=sx; dy-=sy
    def home(s): s.move(-3000,-3000); s.x=s.y=0
    def goto(s,x,y,k=1.0):
        s.move(int((x-s.x)/k),int((y-s.y)/k)); s.x,s.y=x,y
    def click(s,x=None,y=None,k=1.0):
        if x is not None: s.goto(x,y,k)
        s.cmd('mouse_button 1',0.12); s.cmd('mouse_button 0',0.25)
def mk():
    q=Q(); q.home(); return q
if __name__=='__main__':
    q=Q()
    for a in sys.argv[1:]: print(q.cmd(a))
