"""Actual credential codec/NVS adapter with controlled persistence and failures."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import zlib
import copy
import json

root = Path(__file__).resolve().parents[1]
out = root / "artifacts/wardrive-upload-config-checks"
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / "zig-global"), ZIG_LOCAL_CACHE_DIR=str(out / "zig-local"))
exe = out / ("config-check.exe" if os.name == "nt" else "config-check")
console = (root / "s3_main/main/usb_console.c").read_text()
begin = console.index('static void print_upload_profile(void)\n{')
(out / 'upload_profile_impl.h').write_text(console[begin:console.index('static void print_gps(void)\n{', begin)])
subprocess.run([sys.executable, "-m", "ziglang", "cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-I", str(out),
    "-I", str(root / "tools/host_checks/upload_stubs"), "-I", str(root / "tools/host_checks/stubs"),
    "-I", str(root / "s3_main/main"), str(root / "s3_main/main/wardrive_upload_config.c"),
    str(root / "s3_main/main/wardrive_upload_settings.c"), str(root / "tools/host_checks/wardrive_upload_config_check.c"),
    "-o", str(exe)], check=True, env=env)
wire = out / "empty-config.bin"
subprocess.run([str(exe), str(wire)], check=True, env=env)
expected = bytearray(512)
expected[:8] = b"CBUPCFG1"
struct.pack_into("<H", expected, 8, 487)
struct.pack_into("<I", expected, 12, zlib.crc32(expected))
assert wire.read_bytes() == expected
print("Upload configuration independent Python wire/CRC cross-check passed")

json_exe=out/('json-check.exe' if os.name=='nt' else 'json-check')
subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
    '-I',str(root/'s3_main/main'),str(root/'s3_main/main/wardrive_upload_config.c'),
    str(root/'s3_main/main/wardrive_upload_json.c'),str(root/'tools/host_checks/wardrive_upload_json_check.c'),
    '-o',str(json_exe)],check=True,env=env)
blank=json.loads((root/'config_examples/wardrive_upload.example.json').read_text())
checks=0
def check(profile,valid,raw=None):
    global checks
    path=out/'synthetic-profile.json';wire=out/'synthetic-profile.bin'
    wire.unlink(missing_ok=True)
    path.write_bytes(raw if raw is not None else json.dumps(profile,ensure_ascii=True).encode())
    result=json.loads(subprocess.check_output([str(json_exe),str(path),str(wire)],text=True,env=env))
    assert result['valid'] is valid
    if valid:
        mask=sum((1<<i) for i,s in enumerate(('wigle','wdgwars','wardrift')) if profile[s]['enabled'])
        assert result['destinations']==mask
        payload=b''
        for value,size in ((profile['home_wifi']['ssid'],33),(profile['home_wifi']['password'],65),
            (profile['wigle']['api_name'],65),(profile['wigle']['api_token'],129),
            (profile['wdgwars']['token'],65),(profile['wardrift']['token'],129)):
            data=value.encode('utf-8');assert len(data)<size;payload+=data+b'\0'*(size-len(data))
        payload+=bytes([mask]);assert len(payload)==487
        assert wire.read_bytes()[16:503]==payload
    else:assert not wire.exists()
    checks+=1
check(blank,True)
ready=copy.deepcopy(blank);ready['home_wifi']={'ssid':'Lab "café"','password':'fixture-passphrase'}
ready['wigle']={'enabled':True,'api_name':'fixture-name','api_token':'fixture-token'}
ready['wdgwars']={'enabled':True,'token':'a'*64};ready['wardrift']={'enabled':True,'token':'fixture-token'}
check(ready,True);check(ready,True,json.dumps(ready,ensure_ascii=False).encode())
for ssid in ('A'*32,'é'*16,'Fixture \U0001f4e1'):
    p=copy.deepcopy(ready);p['home_wifi']['ssid']=ssid;check(p,True)
for password in ('','A'*63,'a'*64,'slash\\quote"'):
    p=copy.deepcopy(ready);p['home_wifi']['password']=password;check(p,True)
for group,key,value in [('home_wifi','ssid','A'*33),('home_wifi','ssid','é'*17),('home_wifi','ssid',''),
    ('home_wifi','password','short'),('home_wifi','password','z'*64),('home_wifi','password','A'*65),
    ('wigle','api_name','bad:colon'),('wigle','api_token',''),('wdgwars','token','x'*64),('wardrift','token','with space'),
    ('home_wifi','ssid','control\n'),('home_wifi','ssid','hidden\0suffix'),('wigle','enabled','true'),('wdgwars','enabled',1)]:
    p=copy.deepcopy(ready);p[group][key]=value;check(p,False)
for group in ('home_wifi','wigle','wdgwars','wardrift'):
    p=copy.deepcopy(ready);p[group]['extra']='value';check(p,False)
    for key in ready[group]:
        p=copy.deepcopy(ready);del p[group][key];check(p,False)
for value in (None,[],{},'text',1):check(value,False)
p=copy.deepcopy(ready);p['schema']='unknown';check(p,False)
p=copy.deepcopy(ready);p['extra']=True;check(p,False)
raw=json.dumps(ready).encode()
for data in (raw[:-1],raw+b' trailing',raw+b'\0',b' '*4097,raw.replace(b'"schema":',b'"schema":"duplicate","schema":'),
    raw.replace(b'"api_name":',b'"api\\u005fname":'),raw.replace(b'Lab \\"caf\\u00e9\\"',b'bad\\ud800'),
    raw.replace(b'fixture-token',b'bad\xc0\xaf',1)):
    check(None,False,data)
print(f'Upload JSON: {checks} strict-schema, UTF-8/escape, field-limit, readiness and canonical-wire cases passed; secret-free diagnostics verified')
