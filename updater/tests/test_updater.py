"""Exercise transactions only inside TemporaryDirectory; never touch installed OBS."""
import copy,ctypes,hashlib,json,os,subprocess,sys,tempfile,time,zipfile
from pathlib import Path

check,helper=map(lambda p:Path(p).resolve(),sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='wasurene-updater-test-') as folder:
 root=Path(folder)
 def call(request,ok=True):
  path=root/'request.json';path.write_text(json.dumps(request),encoding='utf-8')
  p=subprocess.run([str(check),str(path)],capture_output=True,text=True,timeout=15)
  assert (p.returncode==0)==ok,(request,p.stdout,p.stderr)
  return json.loads(p.stdout) if ok else p.stderr
 for current,candidate,expected in [('0.2.9','v0.2.10',True),('0.2.6','v0.2.6',False),('0.2.6','v0.2.5',False),('0.1.5','native-v0.1.6',True)]:
  assert call({'op':'newer','current':current,'candidate':candidate})['newer']==expected
 for candidate in ['v0.2.7-test','v0.2','v1.2.3.4','v1.-2.3']:
  call({'op':'newer','current':'0.2.6','candidate':candidate},False)
 release={'tag_name':'v0.2.7','draft':False,'prerelease':False,'assets':[{'name':'wasurene-input-display-windows.zip','browser_download_url':'https://github.com/MIGIASHI/wasurene-input-display/releases/download/v0.2.7/wasurene-input-display-windows.zip','digest':'sha256:'+'a'*64,'size':100}]}
 request={'op':'release','repo':'MIGIASHI/wasurene-input-display','asset':'wasurene-input-display-windows.zip','release':release}
 assert call(request)['tag']=='v0.2.7'
 for field,value in [('digest',''),('browser_download_url','https://example.com/plugin.zip'),('size',2**32)]:
  bad=copy.deepcopy(request);bad['release']['assets'][0][field]=value;call(bad,False)
 bad=copy.deepcopy(request);bad['release']['prerelease']=True;call(bad,False)
 for data in [b'',b'abc',b'wasurene'*10000]:
  path=root/'hash.bin';path.write_bytes(data);assert call({'op':'hash','path':str(path)})['hash']==hashlib.sha256(data).hexdigest()
 call({'op':'cancel'},False)
 for url in ['http://github.com/test','https://example.com/','https://github.com.evil.invalid/']:
  call({'op':'unsafe-url','url':url},False)
 print('PASS: version order, prerelease/downgrade rejection, official asset URL/digest/size, SHA256 vectors, cancelled requests and untrusted URLs')

 serial=0
 def fixture(plugin,kind='normal',extra=None):
  global serial
  serial+=1;base=root/str(serial);base.mkdir();stage=base/'stage';stage.mkdir()
  if kind=='normal':dll=base/'obs/obs-plugins/64bit'/f'{plugin}.dll';data=base/'obs/data/obs-plugins'/plugin
  else:dll=base/plugin/'bin/64bit'/f'{plugin}.dll';data=base/plugin/'data'
  dll.parent.mkdir(parents=True);data.joinpath('web').mkdir(parents=True)
  dll.write_bytes(b'old DLL');(data/'web/input.html').write_bytes(b'old page')
  config=base/'user-config';config.mkdir();(config/'settings.json').write_bytes(b'keep settings');(config/'history.json').write_bytes(b'keep history')
  files={f'obs-plugins/64bit/{plugin}.dll':b'new DLL',f'data/obs-plugins/{plugin}/web/input.html':b'new page',f'data/obs-plugins/{plugin}/updater/wasurene-update.exe':b'new helper','README.md':b'docs'}
  if extra:files.update(extra)
  archive=stage/'payload.zip'
  with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
   for name,content in files.items():z.writestr(name,content)
  request={'op':'apply','plugin':plugin,'dll':str(dll),'data':str(data),'stage':str(stage),'zip':str(archive)}
  return base,stage,dll,data,config,archive,request
 for plugin in ['wasurene-input-display','sf6-tracker-native']:
  for kind in ['normal','programdata']:
   base,stage,dll,data,config,archive,request=fixture(plugin,kind);call(request)
   assert dll.read_bytes()==b'new DLL' and (data/'web/input.html').read_bytes()==b'new page'
   assert (config/'settings.json').read_bytes()==b'keep settings' and (config/'history.json').read_bytes()==b'keep history'
   base,stage,dll,data,config,archive,request=fixture(plugin,kind);request['fail_after']=2;call(request,False)
   assert dll.read_bytes()==b'old DLL' and (data/'web/input.html').read_bytes()==b'old page' and not (data/'updater/wasurene-update.exe').exists()
   assert (config/'history.json').read_bytes()==b'keep history'
 print('PASS: both plugins and both layouts update only dedicated files, preserve settings/history, and roll back a partial failure')
 for name in ['../escape.txt','data/obs-plugins/wasurene-input-display/../../escape.txt','data/obs-plugins/wasurene-input-display/a:stream','data/obs-plugins/wasurene-input-display/CON.txt','other-plugin.dll','data/obs-plugins/wasurene-input-display/web/INPUT.html']:
  extra={name:b'bad'}
  if name.endswith('INPUT.html'):extra['data/obs-plugins/wasurene-input-display/web/input.html']=b'new page'
  base,stage,dll,data,config,archive,request=fixture('wasurene-input-display',extra=extra);call(request,False);assert dll.read_bytes()==b'old DLL'
 base,stage,dll,data,config,archive,request=fixture('wasurene-input-display')
 request['data']=str(config);call(request,False);assert dll.read_bytes()==b'old DLL'
 base,stage,dll,data,config,archive,request=fixture('wasurene-input-display')
 with zipfile.ZipFile(archive,'a') as z:
  entry=zipfile.ZipInfo('data/obs-plugins/wasurene-input-display/link');entry.create_system=3;entry.external_attr=0o120777<<16;z.writestr(entry,'../../escape')
 call(request,False);assert dll.read_bytes()==b'old DLL'
 print('PASS: traversal, alternate data streams, reserved filenames, other plugins, duplicate case, mismatched data root and symlink ZIPs rejected before writes')

 # Real helper process: it must wait for the original process handle, not just a PID poll.
 for corrupt,locked in [(False,False),(True,False),(False,True)]:
  base,stage,dll,data,config,archive,request=fixture('wasurene-input-display')
  obs=base/'obs/bin/64bit/obs64.exe';obs.parent.mkdir(parents=True,exist_ok=True);obs.write_bytes(b'sandbox OBS placeholder')
  parent=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)'])
  plan={'schema':1,'plugin':'wasurene-input-display','dll':str(dll),'data':str(data),'obs':str(obs),'pid':parent.pid,'digest':hashlib.sha256(archive.read_bytes()).hexdigest(),'version':'v0.2.7'}
  if corrupt:plan['digest']='0'*64
  (stage/'plan.json').write_text(json.dumps(plan),encoding='utf-8')
  lock=None
  if locked:
   api=ctypes.WinDLL('kernel32',use_last_error=True);api.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p];api.CreateFileW.restype=ctypes.c_void_p;api.CloseHandle.argtypes=[ctypes.c_void_p]
   lock=api.CreateFileW(str(dll),0x80000000,1,None,3,0x80,None);assert lock not in [None,ctypes.c_void_p(-1).value]
  process=subprocess.Popen([str(helper),'--test-wait',str(stage)])
  try:
   deadline=time.monotonic()+10
   while not (stage/'ready').exists() and process.poll() is None and time.monotonic()<deadline:time.sleep(.05)
   assert dll.read_bytes()==b'old DLL'
   if not corrupt:assert (stage/'ready').exists() and process.poll() is None
   parent.terminate();parent.wait(timeout=5);code=process.wait(timeout=15)
   assert (code==0)==(not corrupt and not locked)
   assert dll.read_bytes()==(b'new DLL' if code==0 else b'old DLL')
   assert json.loads((stage/'result.json').read_text())['ok']==(code==0)
   assert (config/'settings.json').read_bytes()==b'keep settings' and (config/'history.json').read_bytes()==b'keep history'
  finally:
   if parent.poll() is None:parent.kill();parent.wait()
   if process.poll() is None:process.kill();process.wait()
   if lock:api.CloseHandle(lock)
 print('PASS: real helper waits for process exit; sandbox update succeeds; wrong digest and DLL locked by another process leave installation unchanged')
