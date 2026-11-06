"""Verify the native-pointer ABI policy in source or in the packaged binary manifest."""
import struct
import sys
import zipfile
from pathlib import Path
from xml.etree import ElementTree

ANDROID = 'http://schemas.android.com/apk/res/android'
ATTR = 'allowNativeHeapPointerTagging'

def check_apk(path):
    with zipfile.ZipFile(path) as apk:
        data = apk.read('AndroidManifest.xml')
    strings = []
    offset = 8
    def u16(p): return struct.unpack_from('<H', data, p)[0]
    def u32(p): return struct.unpack_from('<I', data, p)[0]
    def length8(p):
        n = data[p]
        return (((n & 127) << 8) | data[p+1], p+2) if n & 128 else (n, p+1)
    while offset < len(data):
        kind, header, size = struct.unpack_from('<HHI', data, offset)
        assert size >= header >= 8 and offset + size <= len(data), 'Invalid XML chunk'
        if kind == 1: # ResStringPool
            count, flags, start = u32(offset+8), u32(offset+16), u32(offset+20)
            for i in range(count):
                p = offset + start + u32(offset + header + i*4)
                if flags & 0x100:
                    _, p = length8(p)
                    n, p = length8(p)
                    strings.append(data[p:p+n].decode('utf-8'))
                else:
                    n = u16(p); p += 2
                    if n & 0x8000:
                        n = ((n & 0x7fff) << 16) | u16(p); p += 2
                    strings.append(data[p:p+n*2].decode('utf-16le'))
        elif kind == 0x102 and strings[u32(offset+20)] == 'application':
            start, stride, count = u16(offset+24), u16(offset+26), u16(offset+28)
            for i in range(count):
                p = offset + 16 + start + i*stride
                ns, name = u32(p), u32(p+4)
                if ns != 0xffffffff and strings[ns] == ANDROID and strings[name] == ATTR:
                    assert data[p+15] == 0x12 and u32(p+16) == 0, 'Heap pointer tagging must be false'
                    return
            raise AssertionError('Application is missing the guest pointer ABI policy')
        offset += size
    raise AssertionError('Application element not found')

path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'app/src/main/AndroidManifest.xml'
if path.suffix == '.apk':
    check_apk(path)
else:
    application = ElementTree.parse(path).getroot().find('application')
    assert application is not None and application.get('{'+ANDROID+'}'+ATTR) == 'false'
print('Guest pointer ABI: application disables native heap pointer tagging:', path)
