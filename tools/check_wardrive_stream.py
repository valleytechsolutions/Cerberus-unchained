"""Exercise the production observation FIFO and advertisement decoder."""
from pathlib import Path
import subprocess, sys, os, re
root=Path(__file__).resolve().parents[1]
out=root/'artifacts/wardrive-checks/stream-check.exe'
out.parent.mkdir(parents=True,exist_ok=True)
env=dict(os.environ)
env.setdefault('ZIG_GLOBAL_CACHE_DIR',str(out.parent/'zig-global'))
env.setdefault('ZIG_LOCAL_CACHE_DIR',str(out.parent/'zig-local'))
subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
    '-I',str(root/'shared'),str(root/'tools/host_checks/wardrive_stream_check.c'),'-o',str(out)],check=True,env=env)
subprocess.run([str(out)],check=True)

# Compile every registered command into one switch: duplicate numeric values
# must fail even when the affected services pass isolated tests.
protocol=(root/'shared/cerberus_protocol.h').read_text()
enum=re.search(r'typedef enum \{(.*?)\} cerberus_spi_command_t;',protocol,re.S)
assert enum
names=re.findall(r'\b(CERBERUS_SPI_COMMAND_[A-Z0-9_]+)\s*=',enum.group(1))
assert len(names)>100 and len(names)==len(set(names))
for name in ('START','STATUS','READ','STOP','RELEASE','PAUSE','RESUME'):
    assert 'CERBERUS_SPI_COMMAND_WARDRIVE_'+name in names
registry=out.parent/'spi-command-registry.c'
registry.write_text('#include "wardrive_stream.h"\n#undef NDEBUG\n#include <assert.h>\nint main(void) {\n'
    'switch(255U) {\n'+''.join('case '+name+': break;\n' for name in names)+'default: break; }\n'+
    ''.join('assert(CERBERUS_WD_'+name+' == CERBERUS_SPI_COMMAND_WARDRIVE_'+name+');\n'
            for name in ('START','STATUS','READ','STOP','RELEASE','PAUSE','RESUME'))+
    ''.join('assert((unsigned)'+name+' <= UINT8_MAX);\n' for name in names)+
    'assert(CERBERUS_WD_PAUSE_MIN_FIRMWARE == 79U && CERBERUS_C5_FIRMWARE_VERSION >= 79U);\n'
    'assert(CERBERUS_WD_MIN_FIRMWARE >= 78U && CERBERUS_WD_MIN_FIRMWARE <= CERBERUS_C5_FIRMWARE_VERSION);\nreturn 0;\n}\n')
registry_exe=out.parent/'spi-command-registry.exe'
subprocess.run([sys.executable,'-m','ziglang','cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
    '-I',str(root/'shared'),str(registry),'-o',str(registry_exe)],check=True,env=env)
subprocess.run([str(registry_exe)],check=True,env=env)
print(f'SPI registry: {len(names)} distinct command values; Wardriving aliases and capability gate passed.')
