# Office 16.0.20527.20088 (x64) only: offsets are mso40uiWin32Client.dll RVAs.
# Reports PrefetchScope transaction save/restore that is not LIFO per thread.
# Use: TUX_GDB_COMMANDS=tests/prefetch-transaction-guard.gdb TUXOFFICE_GDB_EARLY=1 tests/office-flicker.py ...
set debuginfod enabled off
set pagination off
set print thread-events off
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGSEGV nostop noprint pass
python
# Track PrefetchScope's pending-transaction member (scope+0x60). Add, Remove,
# Update and Moved each save it, point it at a stack local and restore it on
# exit. Report any save/restore that is not LIFO for the calling thread, which
# would mean two threads are inside the same scope at once.
import gdb,struct,collections,time
from pathlib import Path
inf=gdb.selected_inferior()
pid=inf.pid
mods=[]
lows={}
for l in Path('/proc/%d/maps'%pid).read_text().splitlines():
 f=l.split()
 if len(f)>=6 and f[5].lower().endswith(('.dll','.exe','.so','.drv')):
  a,b=(int(x,16) for x in f[0].split('-'))
  n=f[5].rsplit('/',1)[-1]
  if n.lower().endswith('.so'):
   if 'x' in f[1]:mods.append((a,b,n))
  else:lows[n]=min(a,lows.get(n,a))
for n,a in lows.items():
 # Wine may copy PE sections into anonymous memory; use the image size
 try:
  pe=struct.unpack('<I',inf.read_memory(a+0x3c,4))[0]
  mods.append((a,a+struct.unpack('<I',inf.read_memory(a+pe+0x50,4))[0],n))
 except Exception:pass
def base_of(name):
 return min(int(l.split('-')[0],16) for l in Path('/proc/%d/maps'%pid).read_text().splitlines() if name in l.lower())
base=base_of('mso40uiwin32client.dll')
def reg(n):return int(gdb.parse_and_eval('$'+n))
def read(a):return struct.unpack('<Q',inf.read_memory(a,8))[0]
def tid():
 try:return read(reg('gs_base')+0x48)
 except Exception:return -1
def where(a):
 for lo,hi,n in mods:
  if lo<=a<hi:
   # PE images map from their base; report RVA for mso40ui so it can be symbolized
   return n+('+'+hex(a-base) if 'mso40ui' in n.lower() else '@'+hex(a))
 return None
def comm():
 try:return Path('/proc/%d/task/%d/comm'%(pid,gdb.selected_thread().ptid[1])).read_text().strip()
 except Exception:return '?'
def scan(limit=0x1000):
 sp=reg('rsp');out=[]
 try:data=bytes(inf.read_memory(sp,limit))
 except Exception:return out
 for i in range(0,len(data),8):
  v=struct.unpack_from('<Q',data,i)[0]
  w=where(v)
  if w:out.append(w)
 return out[:32]
history=collections.defaultdict(lambda:collections.deque(maxlen=48))
stacks=collections.defaultdict(list)
reports=[0]
counts=collections.Counter()
def report(kind,member,detail):
 reports[0]+=1
 if reports[0]>12:return
 print('TUX OVERLAP',kind,hex(member),detail,'lwp',gdb.selected_thread().ptid[1],flush=True)
 print('TUX HISTORY',[(e[0],hex(e[1]),e[2],hex(e[3]),hex(e[4])) for e in history[member]],flush=True)
 for e in stacks[member]:
  print('TUX ACTIVE tid',hex(e[0]),e[3],hex(e[1]),'comm',e[5],'stack',e[4],flush=True)
 print('TUX STACK current tid',hex(tid()),'comm',comm(),scan(),flush=True)
class Point(gdb.Breakpoint):
 def __init__(self,off,fn,set_,mreg,vreg):
  super().__init__('*'+hex(base+off),internal=True)
  self.fn,self.set_,self.mreg,self.vreg=fn,set_,mreg,vreg
 def stop(self):
  member=reg(self.mreg);value=reg(self.vreg)
  try:cur=read(member)
  except Exception:cur=0
  t=tid();lwp=gdb.selected_thread().ptid[1]
  counts[self.fn,self.set_]+=1
  history[member].append((t,value,self.fn+(' set' if self.set_ else ' restore'),cur,reg('rsp')))
  st=stacks[member]
  if self.set_:
   if st and st[-1][0]!=t:report('enter-while-other-active',member,(self.fn,hex(t)))
   elif st and st[-1][1]!=cur:report('member-changed-before-enter',member,(self.fn,hex(cur),hex(st[-1][1])))
   st.append((t,value,lwp,self.fn,scan(),comm()))
  else:
   if not st:report('restore-without-enter',member,(self.fn,hex(t)))
   elif st[-1][0]!=t:report('restore-while-other-active',member,(self.fn,hex(t)))
   elif st[-1][1]!=cur:report('member-changed-before-restore',member,(self.fn,hex(cur),hex(st[-1][1])))
   for i in range(len(st)-1,-1,-1):
    if st[i][0]==t:del st[i];break
  return False
for off,fn,set_,mreg,vreg in ((0x12e508,'Add',1,'rdi','rax'),(0x12e648,'Add',0,'rdi','r14'),
                              (0x132dc8,'Remove',1,'rsi','rax'),(0x132edf,'Remove',0,'rsi','r15'),
                              (0x116944,'Moved',1,'rsi','rax'),(0x1169aa,'Moved',0,'rsi','rdi'),
                              (0x12dcc5,'Update',1,'r15','rax'),(0x12de4c,'Update',0,'r15','rax')):
 Point(off,fn,set_,mreg,vreg)
class Tree(gdb.Breakpoint):
 def stop(self):
  address=reg('rcx')
  try:
   head=read(address)
   if head and head<0x800000000000:return False
  except Exception:head=None
  print('TUX BAD PREFETCH SET',hex(address),head,'lwp',gdb.selected_thread().ptid[1],'tid',hex(tid()),flush=True)
  print('TUX COUNTS',dict(counts),flush=True)
  for member in list(history)[:8]:
   print('TUX MEMBER',hex(member),'active',[(e[0],e[3],hex(e[1])) for e in stacks[member]],flush=True)
   print('TUX HISTORY',[(e[0],hex(e[1]),e[2],hex(e[3]),hex(e[4])) for e in history[member]],flush=True)
  print('TUX STACK current',scan(),flush=True)
  return True
Tree('*'+hex(base+0x139944),internal=True)
print('TUX DEBUG ATTACHED',pid,hex(base),flush=True)
end
continue
python
print('TUX COUNTS',dict(counts),flush=True)
end
detach
quit
