"""Actual native scoped survey worker and dispatcher, no hardware access."""
from pathlib import Path
import os
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
out=root/"artifacts/channel-survey-session-checks"
out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/"zig-cache"),ZIG_LOCAL_CACHE_DIR=str(out/"zig-local-cache"))
def function(source,signature):
    start=source.index(signature); brace=source.index("{",start); depth=1; end=brace+1
    while depth:
        if source[end]=="{": depth+=1
        if source[end]=="}": depth-=1
        end+=1
    return source[start:end]+"\n"
wifi=(root/"c5_firmware/main/wifi_scanner.c").read_text()
ble=(root/"c5_firmware/main/ble_scanner.c").read_text()
native=function(wifi,"static bool another_radio_operation_is_active(")
native+=function(wifi,"static bool wifi_connection_is_active_locked(")
native+=function(wifi,"void cerberus_wifi_scanner_get_status(")
native+=function(ble,"static bool another_radio_operation_is_active(").replace("another_radio_operation_is_active","ble_other_radio_active")
(out/"survey_admission.h").write_text(native)
dispatch=(root/"c5_firmware/main/main.c").read_text()
start=dispatch.index("    case CERBERUS_SPI_COMMAND_WIFI_CHANNEL_SURVEY_START:")
(out/"survey_dispatch.h").write_text(dispatch[start:dispatch.index("    case CERBERUS_SPI_COMMAND_WIFI_PACKET_MONITOR_SESSION_INFO:",start)])
exe=out/"channel-survey-session.exe"
subprocess.run([sys.executable,"-m","ziglang","cc","-std=c11","-O1","-Wall","-Wextra","-Werror",
    "-I",str(out),"-I",str(root/"shared"),"-I",str(root/"tools/host_checks/stubs"),"-I",str(root/"c5_firmware/main"),
    str(root/"tools/host_checks/channel_survey_session_check.c"),"-o",str(exe)],check=True,env=env)
subprocess.run([str(exe)],check=True,env=env)
