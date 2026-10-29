#
# gbpy.py - drive the emulator core from Python test scripts (ctypes over
# build/libgbpy.so; run `make build/libgbpy.so` first).
#
#   gbpy.load_rom('game.gb')
#   gb = gbpy.GB()
#   gb.run(60); gb.press('A'); gb.shot('out.png')
#
import ctypes, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
lib = ctypes.CDLL(os.path.join(HERE, 'build', 'libgbpy.so'))

lib.gbpy_new.restype = ctypes.c_void_p
for fn in ('gbpy_wram', 'gbpy_hram', 'gbpy_cart_ram'):
    getattr(lib, fn).restype = ctypes.POINTER(ctypes.c_uint8)
    getattr(lib, fn).argtypes = [ctypes.c_void_p]
lib.gbpy_run.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
lib.gbpy_cart_ram_size.argtypes = [ctypes.c_void_p]
lib.gbpy_cart_ram_size.restype = ctypes.c_uint32
lib.gbpy_reset.argtypes = [ctypes.c_void_p]
lib.gbpy_frame.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.gbpy_link_setup.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
lib.gbpy_link_off.argtypes = [ctypes.c_void_p]
lib.gbpy_link_status.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
lib.gbpy_state_size.argtypes = [ctypes.c_void_p]
lib.gbpy_state_size.restype = ctypes.c_uint32
lib.gbpy_state_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.gbpy_state_set.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

BUTTONS = ('A', 'B', 'SELECT', 'START', 'RIGHT', 'LEFT', 'UP', 'DOWN')
LINK_STATES = ('off', 'waiting', 'ready', 'blocks', 'select', 'confirm')

def load_rom(path):
    data = open(path, 'rb').read()
    lib.gbpy_rom(data, len(data))

def mask(*names):
    m = 0
    for n in names:
        m |= 1 << BUTTONS.index(n.upper())
    return m

def write_png(path, w, h, rows):
    # rows: list of bytes, one grey byte per pixel
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw)))
        f.write(chunk(b'IEND', b''))

class GB:
    def __init__(self):
        self.c = lib.gbpy_new()
        assert self.c, 'gbpy_new failed'
        self.ram_size = lib.gbpy_cart_ram_size(self.c)
        self.frames = 0

    def run(self, frames, *buttons):
        err = lib.gbpy_run(self.c, frames, mask(*buttons))
        if err:
            raise RuntimeError('emulation error %d' % (err - 1))
        self.frames += frames

    def press(self, button, hold=6, after=20):
        self.run(hold, button)
        self.run(after)

    def presses(self, seq, hold=6, after=20):
        for b in seq.split():
            self.press(b, hold, after)

    def peek(self, addr, n=1):
        if addr >= 0xff00:
            p = lib.gbpy_hram(self.c)
            base = 0xff00
        else:
            p = lib.gbpy_wram(self.c)
            base = 0xc000
        return bytes(p[addr - base + i] for i in range(n))

    def poke(self, addr, data):
        p = lib.gbpy_wram(self.c)
        for i, b in enumerate(data):
            p[addr - 0xc000 + i] = b

    def cart_ram(self):
        p = lib.gbpy_cart_ram(self.c)
        return bytes(p[i] for i in range(self.ram_size))

    def set_cart_ram(self, data):
        p = lib.gbpy_cart_ram(self.c)
        for i, b in enumerate(data[:self.ram_size]):
            p[i] = b

    def reset(self):
        lib.gbpy_reset(self.c)

    def frame(self):
        buf = ctypes.create_string_buffer(160 * 144)
        lib.gbpy_frame(self.c, buf)
        return buf.raw

    def shot(self, path, scale=2):
        px = self.frame()
        rows = []
        for y in range(144):
            row = bytes(255 - 85 * px[y * 160 + x] for x in range(160) for _ in range(scale))
            rows.extend([row] * scale)
        write_png(path, 160 * scale, 144 * scale, rows)

    def state(self):
        buf = ctypes.create_string_buffer(lib.gbpy_state_size(self.c))
        lib.gbpy_state_get(self.c, buf)
        return buf.raw

    def set_state(self, data):
        assert len(data) == lib.gbpy_state_size(self.c)
        lib.gbpy_state_set(self.c, data)

    def link_setup(self, name, theirs, mine):
        lib.gbpy_link_setup(self.c, bytes(name), bytes(theirs), bytes(mine))

    def link_off(self):
        lib.gbpy_link_off(self.c)

    def link_status(self):
        out = (ctypes.c_int * 4)()
        lib.gbpy_link_status(self.c, out)
        return dict(state=LINK_STATES[out[0]], traded=bool(out[1]), refused=out[2],
                    party_ok=bool(out[3]))
