"""Kauai chunky file (.3cn/.3th) reader/writer. Little-endian, uncompressed output."""
import struct

def ctg(s):  # 'BKGD' -> uint32 as stored (big-endian-packed char constant)
    b = s.encode('latin1'); assert len(b) == 4
    return struct.unpack('>I', b)[0]
def ctgs(v): return struct.pack('>I', v).decode('latin1')

HDR = '<4sIhhhhiiiii'
FL_PACKED = 4

class Chunk:
    def __init__(s, ctg_, cno, data=b'', name=None, kids=None):
        s.ctg = ctg_ if isinstance(ctg_, int) else ctg(ctg_)
        s.cno = cno; s.data = data; s.name = name
        s.kids = kids or []     # list of (chid, ctg:int, cno)
        s.flags = 0
    def add_kid(s, chid, ctg_, cno):
        s.kids.append((chid, ctg_ if isinstance(ctg_, int) else ctg(ctg_), cno))

def _stn(name):  # STN::GetData: osk(int16), length byte, chars, NUL
    b = name.encode('latin1'); assert len(b) < 256
    return struct.pack('<h', 0x0303) + bytes([len(b)]) + b + b'\0'

def _unstn(b):
    n = b[2]
    return b[3:3 + n].decode('latin1')

class Chunky:
    def __init__(s, creator='CHMP'):
        s.chunks = {}; s.creator = creator
    def add(s, ch): s.chunks[(ch.ctg, ch.cno)] = ch; return ch
    def get(s, ctg_, cno):
        return s.chunks[(ctg_ if isinstance(ctg_, int) else ctg(ctg_), cno)]
    def new_cno(s, ctg_):
        c = ctg_ if isinstance(ctg_, int) else ctg(ctg_)
        used = [k[1] for k in s.chunks if k[0] == c]
        return (max(used) + 1) if used else 0

    @staticmethod
    def read(path):
        d = open(path, 'rb').read()
        magic, creator, dver, dverb, bo, osk, fpMac, fpIdx, cbIdx, fpMap, cbMap = struct.unpack_from(HDR, d, 0)
        assert magic == b'CHN2' and bo == 1, "unsupported file"
        g = d[fpIdx:fpIdx + cbIdx]
        bo_, osk_, iloc, bvMac, clocFree, cbFixed = struct.unpack_from('<hhiiii', g, 0)
        data = g[20:20 + bvMac]
        locs = [struct.unpack_from('<ii', g, 20 + bvMac + 8 * i) for i in range(iloc)]
        assert cbFixed == 20
        cf = Chunky(); cf.creator = ctgs(creator)
        for bv, cb in locs:
            e = data[bv:bv + cb]
            c_, cno, fp, lu, ckid, ref = struct.unpack_from('<IIiIHH', e, 0)
            flags, size = lu & 0xff, lu >> 8
            ch = Chunk(c_, cno)
            ch.flags = flags
            for i in range(ckid):
                kc, kn, kchid = struct.unpack_from('<III', e, 20 + 12 * i)
                ch.kids.append((kchid, kc, kn))
            rest = e[20 + 12 * ckid:]
            if rest: ch.name = _unstn(rest)
            raw = d[fp:fp + size]
            ch.data = raw
            cf.add(ch)
        return cf

    def write(s, path):
        heap = bytearray(128)
        placed = {}
        refs = {k: 0 for k in s.chunks}
        for ch in s.chunks.values():
            for chid, kc, kn in ch.kids:
                assert (kc, kn) in s.chunks, "missing kid %s %d" % (ctgs(kc), kn)
                refs[(kc, kn)] += 1
        for k in sorted(s.chunks):
            ch = s.chunks[k]
            placed[k] = len(heap)
            heap += ch.data
        idx = bytearray(); locs = []
        for k in sorted(s.chunks):
            ch = s.chunks[k]
            kids = sorted(ch.kids, key=lambda t: (t[0], t[1], t[2]))
            assert len(set(kids)) == len(kids)
            fl = ch.flags | (2 if refs[k] == 0 else 0)  # unreferenced chunks must be Loners; packed carried raw
            e = struct.pack('<IIiIHH', ch.ctg, ch.cno, placed[k] if ch.data else placed[k],
                            (len(ch.data) << 8) | fl, len(kids), refs[k])
            for chid, kc, kn in kids: e += struct.pack('<III', kc, kn, chid)
            if ch.name is not None: e += _stn(ch.name)
            locs.append((len(idx), len(e))); idx += e
        g = struct.pack('<hhiiii', 1, 0x0303, len(locs), len(idx), -1, 20) + bytes(idx)
        for bv, cb in locs: g += struct.pack('<ii', bv, cb)
        fpIdx = len(heap)
        hdr = struct.pack(HDR, b'CHN2', ctg(s.creator), 5, 4, 1, 0x0303,
                          fpIdx + len(g), fpIdx, len(g), fpIdx + len(g), 0)
        hdr += b'\0' * (128 - len(hdr))
        heap[0:128] = hdr
        open(path, 'wb').write(bytes(heap) + g)
