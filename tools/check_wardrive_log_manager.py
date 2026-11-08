"""Actual manager and leased storage with synthetic card and network boundaries."""
from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[1];out=root/'artifacts/wardrive-log-manager-checks';out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/'zig-global'),ZIG_LOCAL_CACHE_DIR=str(out/'zig-local'))
exe=out/('manager.exe' if os.name=='nt' else 'manager')
console=(root/'s3_main/main/usb_console.c').read_text()
start=console.index('static void print_upload_status(void)\n{')
(out/'upload_status_impl.h').write_text(console[start:console.index('static void print_upload_profile(void)\n{',start)])
start=console.index('static void print_wardrive_logs(void)\n{')
(out/'ward_logs_console_impl.h').write_text(console[start:console.index('static void print_reports(void)\n{',start)])
subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-Wall','-Wextra','-Werror',
    '-include',str(root/'tools/host_checks/upload_stubs/host_time.h'),
    '-I',str(out),'-I',str(root/'tools/host_checks/upload_stubs'),'-I',str(root/'tools/host_checks/stubs'),
    '-I',str(root/'s3_main/main'),'-I',str(root/'shared'),str(root/'tools/host_checks/wardrive_log_manager_check.c'),
    *[str(root/'s3_main/main'/n) for n in ['wardrive_log_manager.c','wardrive_upload_config.c','wardrive_upload_json.c','wardrive_upload_file.c','wardrive_upload_receipt.c','wardrive_upload_reply.c','wardrive_log.c']],
    '-o',str(exe)],check=True,env=env)
meta='WigleWifi-1.6,appRelease=fixture,model=Cerberus,release=fixture,device=Cerberus,display=ST7796,board=ESP32-S3-C5,brand=Cerberus,star=Sol,body=3,subBody=0\n'
columns='MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n'
row='02:00:00:00:00:01,Fixture,WPA2,2026-01-01 12:00:00,6,2437,-55,12.5,-45.5,100,,,,WIFI\n'
# Use the independently generated UNLOC fixture already maintained by the log
# parser suite; do not touch the user's mounted card or recorded identifiers.
import re
header=(root/'s3_main/main/wardrive_core.h').read_text()
start=header.index('#define CERBERUS_WARDRIVE_UNLOCATED_HEADER')
section=header[start:header.index('\n\n',start)]
unloc_header=''.join(bytes(s,'utf8').decode('unicode_escape') for s in re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"',section))
unloc=['']*34;unloc[:5]=['1','2','S3','BLE','02:00:00:00:00:02']
import csv,io,tempfile,json
for mode in ('normal','current_name','unknown','wdgwars_unknown','cancel','sd_fail','network_fail','busy','import_ok','import_invalid','import_save_fail','import_readback_fail','import_remove_fail','import_cancel_after_save',
             'retry_ok','retry_unknown','retry_disabled','retry_network_fail','retry_cancel_before','retry_cancel_send',
             'retry_corrupt','retry_acknowledged','retry_multipart','retry_queued','multipart_cancel'):
    with tempfile.TemporaryDirectory(dir=out,prefix=mode+'-') as td:
        folder=Path(td)/'card/CERBERUS/WARD';folder.mkdir(parents=True)
        (folder/('CERBWardrive001.CSV' if mode=='current_name' else 'WIGLE001.CSV')).write_text(meta+columns+row*(14000 if mode in ('retry_multipart','multipart_cancel') else 10),newline='')
        stream=io.StringIO();stream.write(unloc_header);csv.writer(stream,lineterminator='\n').writerow(unloc)
        (folder/'UNLOC001.CSV').write_text(stream.getvalue(),newline='')
        (folder/'KEEP.TXT').write_text('fixture');(folder/'WIGLE002.TMP').write_text('unfinished fixture')
        (folder/'IGNORED.csv').write_text('not a native log')
        config=folder.parent/'CONFIG';config.mkdir()
        profile=json.loads((root/'config_examples/wardrive_upload.example.json').read_text())
        profile['home_wifi']={'ssid':'JSON fixture AP','password':'json-password'}
        profile['wigle']={'enabled':True,'api_name':'json-name','api_token':'json-token'}
        (config/'UPLOAD.JSON').write_text('invalid {' if mode=='import_invalid' else json.dumps(profile))
        subprocess.run([str(exe),mode],cwd=td,check=True,env=env)
print('Log manager: twenty-five lifecycle/failure/import/retry scenarios passed; no real network/card accessed')
