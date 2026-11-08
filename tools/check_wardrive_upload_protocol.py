"""Independent service contracts, strict JSON, receipts and lossless chunk plans."""
from pathlib import Path
import os, subprocess, sys, json, csv, io
root=Path(__file__).resolve().parents[1]
out=root/'artifacts/wardrive-upload-protocol-checks';out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/'zig-global'),ZIG_LOCAL_CACHE_DIR=str(out/'zig-local'))
exe=out/('protocol.exe' if os.name=='nt' else 'protocol')
subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
    '-I',str(root/'s3_main/main'),'-I',str(root/'shared'),str(root/'tools/host_checks/wardrive_upload_protocol_check.c'),
    *[str(root/'s3_main/main'/name) for name in ['wardrive_upload_reply.c','wardrive_upload_receipt.c','wardrive_upload_file.c','wardrive_log.c']],
    '-o',str(exe)],check=True,env=env)
fixture=out/'response.json'; count=0
def reply(body,service=0,status=200,complete=True,expected=5):
    global count
    fixture.write_bytes(body if isinstance(body,bytes) else json.dumps(body).encode())
    p=subprocess.run([str(exe),'reply',str(fixture),str(service),str(status),str(int(complete))],capture_output=True,text=True,check=True)
    fields=p.stdout.split(); assert int(fields[0])==expected,(body,fields);count+=1;return fields
wigle={'success':True,'results':{'transids':[{'transId':'test-123'}]}}
assert reply(wigle,expected=2)[-1]=='test-123'
assert reply({'ok':True,'imported':4,'no_gps':2,'bad_rows':1},1,expected=3)[1:4]==['4','3','1']
for job in (1,123456,2**64-1,'123456'):
    result=reply({'ok':True,'job_id':job,'status':'queued'},1,202,expected=2)
    assert result[-1]==str(job) and result[3]=='0'
for job in (None,True,False,0,-1,1.5,2**64,'','0','-1','1.0','1e2','a123','../123','1\nHeader','1'*21):
    reply({'ok':True,'job_id':job},1,202)
for body in ({'ok':False,'job_id':1},{'ok':True,'job_id':1,'error':'failure'},
             {'job_id':1},b'{"ok":true,"job_id":1,"job_id":2}'):
    reply(body,1,202)
reply({'ok':True,'job_id':1},1,202,complete=False)
for service in (0,2):reply({'ok':True,'job_id':1},service,202)
reply({'accepted':True,'flags':['signature_missing']},2,expected=3)
for service,key in enumerate(('success','ok','accepted')):
    for status in (200,400,401,403,413,422,429):reply({key:False},service,status,expected=4)
    for body in ({key:'true'},{'wrapped':{key:True}},{key:True,'error':'failure'},[True],True,None):reply(body,service)
    for status in (0,201,202,204,301,302,307,401,403,429,500,503):reply(wigle if service==0 else {key:True,'imported':0},service,status)
    reply(wigle if service==0 else {key:True,'imported':0},service,complete=False)
reply({'success':True})
reply({'success':True,'results':{'transids':[]}})
reply({'success':True,'results':{'transids':[{'transId':'x'},{'transId':'y'}]}})
reply({'success':True,'results':{'transids':[{'transId':'x\nHeader'}]}})
reply({'ok':True},1)
for imported in (-1,1.5,'5',True,2**32):reply({'ok':True,'imported':imported},1)
for body in (b'{"accepted":true,"accepted":false}',b'{"accepted":false,"accepted":true}',
    b'{"acc\\u0065pted":false,"accepted":true}',b'{"accepted":true} trailing',b'{"accepted":true,}',
    b'{"accepted":true,"n":01}',b'{"accepted":true,"n":1e}',b'{"accepted":true,"s":"\\uD800"}',
    b'{"accepted":true,"s":"\xc0\x80"}',b'{"accepted":true}\x00',b'<html>challenge</html>'):
    reply(body,2)
valid=json.dumps(wigle).encode()
for n in range(len(valid)):reply(valid[:n])
reply(b'{"accepted":true,"x":'+b'['*12+b'0'+b']'*12+b'}',2)
reply({'accepted':True,'padding':'a'*4096},2)
meta=b'WigleWifi-1.6,appRelease=fixture,model=Cerberus,release=fixture,device=Cerberus,display=ST7796,board=ESP32-S3-C5,brand=Cerberus,star=Sol,body=3,subBody=0\n'
columns=b'MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n'
row=b'02:00:00:00:00:01,"Lab, ""quoted""",WPA2,2026-11-04 12:00:00,6,2437,-55,12.5,-45.5,100,,,,WIFI\n'
data=meta+columns+row*25000;file=out/'capture.csv';file.write_bytes(data)
def plan(cancel=False):
    p=subprocess.run([str(exe),'file',str(file),str(int(cancel))],capture_output=True,text=True,check=True)
    return list(map(int,p.stdout.split()))
p=plan();assert p[0]==1 and p[1]>=3 and p[2]==25000 and p[3]==len(meta+columns)
offsets=p[4:];assert len(offsets)==p[1]+1 and offsets[0]==len(meta+columns) and offsets[-1]==len(data)
reassembled=b''
for start,end in zip(offsets,offsets[1:]):
    part=data[start:end];assert part.endswith(b'\n') and len(part)<=1024*1024
    parsed=list(csv.reader(io.StringIO((meta+columns+part).decode())))
    assert all(len(r)==14 for r in parsed[1:]);reassembled+=part
assert reassembled==data[len(meta+columns):]
assert plan(True)[0]==0
file.write_bytes(data[:-1]);assert plan()[0]==0
print(f'Upload protocols: {count} service/JSON cases, receipt bit corruption and lossless multi-part CSV passed')
