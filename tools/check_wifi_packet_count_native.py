"""Actual C5 Packet Count service, common RX/status, and production dispatch."""
from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[1]
out=root/'artifacts/wifi-packet-count-native-checks'
out.mkdir(parents=True,exist_ok=True)
def function(source,signature):
 start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
 while depth:
  if source[end]=='{':depth+=1
  if source[end]=='}':depth-=1
  end+=1
 return source[start:end]+'\n'
source=(root/'c5_firmware/main/wifi_scanner.c').read_text()
(out/'count_native_callback.h').write_text(function(source,'static void wifi_promiscuous_callback('))
(out/'count_native_admission.h').write_text(function(source,'void cerberus_wifi_scanner_get_status('))
source=(root/'c5_firmware/main/main.c').read_text();start=source.index('    /* BEGIN PACKET COUNT DISPATCH */')
(out/'count_native_dispatch.h').write_text(source[start:source.index('    /* END PACKET COUNT DISPATCH */',start)])
exe=out/'count-native.exe';env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/'zig-cache'),ZIG_LOCAL_CACHE_DIR=str(out/'zig-local-cache'))
subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-I',str(out),'-I',str(root/'shared'),'-I',str(root/'tools/host_checks/stubs'),'-I',str(root/'c5_firmware/main'),str(root/'tools/host_checks/wifi_packet_count_native_check.c'),str(root/'shared/wifi_packet_count.c'),str(root/'shared/wifi_capture_ring.c'),str(root/'shared/wifi_pcap.c'),'-o',str(exe)],check=True,env=env)
subprocess.run([str(exe)],check=True,env=env)
