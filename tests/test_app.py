"""Real standalone API, persistence and isolation checks. No input is injected."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

EXE=str(Path(sys.argv.pop(1)).resolve())
class AppTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='input-display-')
        self.data=Path(self.temp.name)/'設定 data'
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',0));self.port=sock.getsockname()[1]
        self.base=f'http://127.0.0.1:{self.port}'
        self.start()
    def start(self):
        self.proc=subprocess.Popen([EXE,'--port',str(self.port),'--data-dir',str(self.data)],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        until=time.monotonic()+10
        while time.monotonic()<until:
            if self.proc.poll() is not None:self.fail('app exited during startup')
            try:
                if self.request('/api/status')[0]==200:return
            except (URLError,ConnectionError):pass
            time.sleep(.05)
        self.fail('app startup timeout')
    def request(self,path,body=None,headers=None):
        data=None if body is None else json.dumps(body).encode()
        hdr={'Content-Type':'application/json',**(headers or {})}
        try:
            with urlopen(Request(self.base+path,data=data,headers=hdr),timeout=3) as response:return response.status,response.read()
        except HTTPError as error:return error.code,error.read()
    def stop(self):
        self.proc.communicate(b'stop\n',timeout=5)
        self.assertEqual(self.proc.returncode,0)
    def tearDown(self):
        if self.proc.poll() is None:
            try:self.stop()
            except Exception:self.proc.kill();self.proc.wait(timeout=5)
        self.temp.cleanup()
    def test_page_and_input_apis(self):
        code,body=self.request('/input?setup=1');self.assertEqual(code,200)
        self.assertIn(b'WASURENE. Input Display',body)
        for word in [b'leverless',b'/api/rush',b'SF6',b'value="stick"']:
            self.assertNotIn(word,body)
        self.assertIn('pads',json.loads(self.request('/api/input')[1]))
        self.assertIn('pressed',json.loads(self.request('/api/keys?codes=')[1]))
        motion=json.loads(self.request('/api/keys?codes=87&motion=1')[1])['motion']
        self.assertEqual(set(motion),{'available','x','y'})
        self.assertEqual(self.request('/settings.json')[0],404)
    def test_replacement_and_restart(self):
        a={'type':'pad','positions':{'a0':[20,10]},'map':[2,3]}
        self.assertEqual(self.request('/api/input-settings',a)[0],200)
        b={'type':'keyboard','map':[87,1,256],'names':['W','ML','W↑'],'positions':{}}
        self.assertEqual(self.request('/api/input-settings',b)[0],200)
        self.assertEqual(json.loads(self.request('/api/input-settings')[1]),b)
        self.stop();self.start()
        self.assertEqual(json.loads(self.request('/api/input-settings')[1]),b)
        self.assertEqual(json.loads((self.data/'settings.json').read_text(encoding='utf-8')),b)
        self.assertEqual(sorted(p.name for p in self.data.iterdir()),['settings.json'])
    def test_bad_keys_and_settings(self):
        for codes in ['0','255','260','word','1,,2',','.join(['65']*33)]:
            self.assertEqual(self.request('/api/keys?codes='+codes)[0],400,codes)
        for data in [[],{'type':'leverless'},{'type':'stick'}]:
            self.assertEqual(self.request('/api/input-settings',data)[0],400)
        self.assertEqual(self.request('/api/input-settings',{'type':'pad','large':'x'*17000})[0],413)
    def test_local_origin_only(self):
        for path,body in [('/api/input',None),('/api/input-settings',{'type':'pad'}),('/api/quit',{})]:
            self.assertEqual(self.request(path,body,{'Origin':'https://example.com'})[0],403)
        self.assertEqual(self.request('/api/input',headers={'Host':'example.com'})[0],403)
        self.assertEqual(self.request('/api/input',headers={'Sec-Fetch-Site':'cross-site'})[0],403)
        self.assertEqual(self.request('/api/status')[0],200)
    def test_stop_with_idle_connection_and_restart(self):
        idle=socket.create_connection(('127.0.0.1',self.port),timeout=3)
        idle.sendall(b'GET /input HTTP/1.1\r\nHost:')
        self.stop();idle.close();self.start()
        self.assertEqual(self.request('/api/status')[0],200)
if __name__=='__main__':unittest.main()
