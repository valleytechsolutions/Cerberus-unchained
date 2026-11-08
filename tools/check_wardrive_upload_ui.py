"""Render the actual log/upload UI and exercise its touch routes without a board."""
from pathlib import Path
import os,struct,subprocess,sys,zlib
root=Path(__file__).resolve().parents[1];out=root/'artifacts/wardrive-upload-ui-checks';out.mkdir(parents=True,exist_ok=True)
ui=(root/'s3_main/main/ui.c').read_text(encoding='utf8')
def between(s,a,b):
    start=s.index(a);return s[start:s.index(b,start)]
helpers=between(ui,'static const cerberus_ui_widget_theme_t ui_colors =','static esp_err_t draw_detail_page(void);')
helpers+=between(ui,'static uint16_t text_width(','static void refresh_capability_map(')
(out/'upload_draw_helpers.h').write_text(helpers)
display=(root/'s3_main/main/display.c').read_text();start=display.index('static const uint8_t *font5x7(')
(out/'display_font.h').write_text(display[start:display.index('\nesp_err_t ',start)])
env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(out/'zig-global'),ZIG_LOCAL_CACHE_DIR=str(out/'zig-local'))
exe=out/('ui.exe' if os.name=='nt' else 'ui')
subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function',
    '-I',str(out),'-I',str(root/'tools/host_checks/upload_stubs'),'-I',str(root/'tools/host_checks/stubs'),'-I',str(root/'s3_main/main'),'-I',str(root/'shared'),
    str(root/'tools/host_checks/wardrive_upload_ui_check.c'),*[str(root/'s3_main/main'/n) for n in ['ui_widgets.c','keyboard.c','wardrive_upload_config.c','wardrive_upload_reply.c','wardrive_log.c']],
    '-o',str(exe)],check=True,env=env)
subprocess.run([str(exe),str(out/'upload')],check=True,env=env)
def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
for p in out.glob('upload-*.ppm'):
    raw=p.read_bytes();header=b'P6\n320 480\n255\n';assert raw.startswith(header);rgb=raw[len(header):]
    assert len(rgb)==320*480*3
    p.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',320,480,8,2,0,0,0))+
        chunk(b'IDAT',zlib.compress(b''.join(b'\0'+rgb[y*960:(y+1)*960] for y in range(480))))+chunk(b'IEND',b''))
print(out)
