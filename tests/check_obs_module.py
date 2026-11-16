"""Load the actual plugin in official OBS, exercise APIs, unload and reload."""
import ctypes as c
import json
import os
from pathlib import Path
import sys
import tempfile
from urllib.error import URLError
from urllib.request import Request, urlopen

obs_root,package=(Path(p).resolve() for p in sys.argv[1:])
obs_bin=obs_root/'bin/64bit'
dll=package/'obs-plugins/64bit/wasurene-input-display.dll'
data=package/'data/obs-plugins/wasurene-input-display'
assert dll.is_file() and (data/'web/input.html').is_file()
assert not list(package.rglob('*.exe')), 'Applications/test executables must not be packaged'
def api(path,body=None):
    req=Request('http://127.0.0.1:17861'+path,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'})
    with urlopen(req,timeout=5) as response:return json.loads(response.read())
with os.add_dll_directory(str(obs_bin)),tempfile.TemporaryDirectory(prefix='input-display-obs-') as config:
    obs=c.CDLL(str(obs_bin/'obs.dll'))
    obs.obs_startup.argtypes=[c.c_char_p,c.c_char_p,c.c_void_p];obs.obs_startup.restype=c.c_bool
    obs.obs_shutdown.argtypes=[];obs.obs_shutdown.restype=None
    obs.obs_open_module.argtypes=[c.POINTER(c.c_void_p),c.c_char_p,c.c_char_p];obs.obs_open_module.restype=c.c_int
    obs.obs_init_module.argtypes=[c.c_void_p];obs.obs_init_module.restype=c.c_bool
    obs.obs_get_version_string.restype=c.c_char_p
    settings={'type':'keyboard','map':[87,1,256],'names':['W','ML','W↑'],'positions':{}}
    for attempt in range(2):
        assert obs.obs_startup(b'en-US',config.encode(),None)
        try:
            module=c.c_void_p()
            assert obs.obs_open_module(c.byref(module),str(dll).encode(),str(data).encode())==0
            assert obs.obs_init_module(module)
            assert api('/api/status')['app']=='WASURENE. Input Display'
            assert 'pads' in api('/api/input')
            assert 'pressed' in api('/api/keys?codes=87,1,256')
            if attempt==0:assert api('/api/input-settings',settings)['ok']
            assert api('/api/input-settings')==settings
            print('PASS: OBS '+obs.obs_get_version_string().decode()+' plugin load, APIs and settings; pass '+str(attempt+1))
        finally:obs.obs_shutdown()
        try:
            api('/api/status')
        except (URLError,ConnectionError):pass
        else:raise AssertionError('Input service still runs after OBS shutdown')
    print('PASS: plugin-only packages; service stops with OBS and settings survive restart')
