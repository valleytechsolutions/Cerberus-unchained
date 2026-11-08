"""Compile the actual dual-target receive engine against fault-injectable SDK boundaries."""
from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[1]
out=root/'artifacts/wardrive-radio-checks';stubs=out/'stubs'
for name in ('esp_err.h','esp_heap_caps.h','esp_random.h','esp_timer.h','esp_wifi.h','sdkconfig.h','freertos/FreeRTOS.h',
             'freertos/task.h','host/ble_hs.h','host/util/util.h','wifi_scanner.h','ble_scanner.h'):
    p=stubs/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('/* Fixture SDK boundary. */\n')
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/'zig-global'),ZIG_LOCAL_CACHE_DIR=str(out/'zig-local'))
for tick_ms in (1,10):
    for target in ('ESP32S3','ESP32C5'):
        exe=out/(target+'-'+str(tick_ms)+'.exe')
        subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
            '-DFIXTURE_TICK_MS='+str(tick_ms),'-DCONFIG_IDF_TARGET_'+target+'=1','-DCONFIG_BT_NIMBLE_ENABLED=1',
            '-DFIXTURE_EXT_ADV='+str(int(target=='ESP32C5')),
            '-I',str(stubs),'-I',str(root/'shared'),'-I',str(root/'s3_main/main'),
            str(root/'tools/host_checks/wardrive_radio_check.c'),'-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True)
    exe=out/('writer-'+str(tick_ms)+'.exe')
    subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
        '-DFIXTURE_TICK_MS='+str(tick_ms),'-I',str(stubs),'-I',str(root/'shared'),'-I',str(root/'s3_main/main'),
        str(root/'tools/host_checks/wardrive_stream_worker_check.c'),str(root/'s3_main/main/wardrive_core.c'),
        str(root/'s3_main/main/wardrive_geofence.c'),'-lm',
        '-o',str(exe)],check=True,env=env)
    subprocess.run([str(exe)],check=True)
