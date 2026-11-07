"""Compile the real ESP HTTP adapter against controlled SDK boundaries."""
from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[1];out=root/'artifacts/wardrive-upload-net-checks';out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/'zig-global'),ZIG_LOCAL_CACHE_DIR=str(out/'zig-local'))
exe=out/('network.exe' if os.name=='nt' else 'network')
subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-DCERBERUS_APP_VERSION="fixture"',
    '-I',str(root/'tools/host_checks/upload_stubs'),'-I',str(root/'tools/host_checks/stubs'),'-I',str(root/'s3_main/main'),'-I',str(root/'shared'),
    str(root/'tools/host_checks/wardrive_upload_net_check.c'),*[str(root/'s3_main/main'/n) for n in ['wardrive_upload_net.c','wardrive_upload_config.c','wardrive_upload_file.c','wardrive_upload_reply.c','wardrive_log.c']],
    '-o',str(exe)],check=True,env=env)
csv=out/'synthetic.csv';csv.write_text('WigleWifi-1.6,appRelease=fixture,model=Cerberus,release=fixture,device=Cerberus,display=ST7796,board=ESP32-S3-C5,brand=Cerberus,star=Sol,body=3,subBody=0\n'
    'MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n'
    '02:00:00:00:00:01,Fixture,WPA2,2026-01-01 12:00:00,6,2437,-55,12.5,-45.5,100,,,,WIFI\n',newline='')
subprocess.run([str(exe),str(csv)],check=True,env=env)
subprocess.run([str(exe),str(csv),'retry-clock'],check=True,env=env)
