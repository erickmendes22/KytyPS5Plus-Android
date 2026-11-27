"""Versioned RDNA1 graphics export contract for the finite P8 cone.

Input order: original admitted code, launch (EXEC u64, MODE u32), initial
SGPR and VGPR bytes, optional interpolation LDS, then guest memory regions.
The caller must prove the launch and input versions independently. Output is
lane-major, selected enabled channels in component order, as raw u32 words.
No native shader translation or expected-output field participates.
"""
from pathlib import Path
import struct
import subprocess
import tempfile


def require(ok, message):
    if not ok:
        raise ValueError(message)


def words(data):
    require(isinstance(data, bytes) and data and len(data) % 4 == 0, 'invalid register bytes')
    return struct.unpack('<%dI' % (len(data)//4), data)


def execute(executable, params, inputs):
    require(set(params) == {'wave', 'addresses', 'lds', 'target', 'enabled', 'compressed', 'lanes'}, 'invalid export parameters')
    wave = params['wave']
    target, enabled, compressed, lanes = (params[k] for k in ('target', 'enabled', 'compressed', 'lanes'))
    require(wave in (32, 64) and type(target) is int and 0 <= target <= 63 and
            type(enabled) is int and 0 < enabled < 16 and type(compressed) is int and compressed in (0, 1) and
            type(lanes) is int and 0 < lanes < 1 << wave and type(params['lds']) is bool,
            'invalid export selection')
    start = 5 if params['lds'] else 4
    addresses = params['addresses']
    require(len(inputs) == start + len(addresses) and all(isinstance(b, bytes) and b for b in inputs), 'incomplete graphics operands')
    code, launch, sg, vg = inputs[:4]
    require(len(code) % 4 == 0 and len(launch) == 12, 'invalid code/launch')
    execmask, mode = struct.unpack('<QI', launch)
    require(0 < execmask < 1 << wave and not lanes & ~execmask, 'export outside launch')
    sg, vg = words(sg), words(vg)
    require(len(sg) <= 106 and len(vg) % wave == 0 and 0 < len(vg)//wave <= 256, 'invalid wave ABI')
    lds = inputs[4] if params['lds'] else None
    require(lds is None or len(lds) <= 65536 and len(lds) % 4 == 0, 'invalid interpolation LDS')
    segments = sorted(zip(addresses, inputs[start:]))
    require(all(type(a) is int and 0 <= a <= 2**64-len(b) for a,b in segments), 'invalid guest memory')
    require(all(a+len(b) <= c for (a,b),(c,_) in zip(segments,segments[1:])), 'overlapping guest memory')
    pc = 0x10000
    while any(a < pc+len(code) and pc < a+len(b) for a,b in segments):
        pc += 0x100000
        require(pc+len(code) < 2**64, 'guest code address overflow')
    with tempfile.TemporaryDirectory(prefix='p8-export-') as d:
        root = Path(d)
        f = root/'guest.bin'; f.write_bytes(code)
        lines = [f'P8ISA2 rdna1 {pc:x} {execmask:x} {mode:x} {wave} {len(sg)} {len(vg)//wave} 1000000 "{f}"',
                 ' '.join(f'{v:x}' for v in sg), ' '.join(f'{v:x}' for v in vg), str(len(segments))]
        for i,(a,b) in enumerate(segments):
            f = root/f'mem{i}.bin'; f.write_bytes(b); lines.append(f'{a:x} "{f}"')
        lines.append('0')
        if lds is None:
            lines.append('"-"')
        else:
            f = root/'lds.bin'; f.write_bytes(lds); lines.append(f'"{f}"')
        f = root/'manifest.txt'; f.write_text('\n'.join(lines)+'\n')
        result = subprocess.run([str(Path(executable).resolve()), str(f)], capture_output=True, text=True, timeout=40)
    require(result.returncode == 0, 'guest graphics executor: '+result.stderr[-1000:])
    rows = result.stdout.splitlines()
    require(sum(x.startswith('P8ISA_RESULT1 ') for x in rows) == 1, 'missing graphics result header')
    values = {}
    for row in rows:
        if not row.startswith('GFXEXP '):
            continue
        t, e, c, v, done, active, *raw = [int(x,16) for x in row.split()[1:]]
        require(0 <= t <= 63 and 0 < e <= 15 and c in (0,1) and v in (0,1) and done in (0,1) and
                0 <= active < 1 << wave and len(raw) == 4*active.bit_count() and
                all(0 <= x <= 0xffffffff for x in raw), 'invalid graphics export packet')
        if t != target:
            continue
        # No silent truncation of unexpected/duplicate lanes or channel masks.
        require(e == enabled and c == compressed and not active & ~lanes, 'export selection mismatch')
        for i,lane in enumerate(j for j in range(wave) if active >> j & 1):
            require(lane not in values, 'duplicate target/lane export')
            values[lane] = raw[i*4:i*4+4]
    require(set(values) == {i for i in range(wave) if lanes >> i & 1}, 'missing selected export lanes')
    # The RDNA1 callback splits compressed pairs into zero-extended half
    # components. No format-specific framebuffer conversion is performed.
    if compressed:
        require(all(values[l][c] <= 0xffff for l in values for c in range(4) if enabled >> c & 1),
                'invalid compressed half component')
    return b''.join(struct.pack('<I', values[l][c]) for l in sorted(values) for c in range(4) if enabled >> c & 1)
