"""Standard MIDI file -> 3DMM MIDS stream. Faithful port of MIDS::PmidsReadNative / MSTP::FGetEvent (kauai/src/midi.cpp).
MIDS = repeated [variable-length delta ms][raw MIDI event bytes], tracks merged, tempo folded into the deltas."""

import struct


class _Track:
    def __init__(s, data):
        s.d = data
        s.pos = 0
        s.ts = 0
        s.status = 0
        s.tempo = 500000
        s.cur = None

    def _var(s):
        v = 0
        while True:
            if s.pos >= len(s.d):
                return None
            b = s.d[s.pos]
            s.pos += 1
            v = (v << 7) + (b & 0x7F)
            if not b & 0x80:
                return v & 0xFFFFFFFF

    def next(s):
        """-> (ts, send_bytes, lwTempo) ; send_bytes == b'' means tempo change. None at end/failure."""
        d, lim = s.d, len(s.d)
        pos0 = s.pos
        while True:
            delta = s._var()
            if delta is None or s.pos >= lim:
                break
            s.ts = (s.ts + delta) & 0xFFFFFFFF
            b = d[s.pos]
            if b & 0x80:
                s.status = b
                s.pos += 1
            st = s.status
            hi = st & 0xF0
            send = bytes([st])
            if hi in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                n = 2
            elif hi in (0xC0, 0xD0):
                n = 1
            elif hi == 0xF0:
                lo = st & 0x0F
                if lo == 2:
                    n = 2
                elif lo in (1, 3):
                    n = 1
                elif lo in (6, 8, 10, 11, 12, 14):
                    s.status = 0
                    return (s.ts, send, s.tempo)
                elif lo in (0, 7):
                    c = s._var()
                    if c is None or not (1 <= c <= lim - s.pos):
                        break
                    s.pos += c
                    continue
                elif lo == 0xF:
                    if s.pos >= lim:
                        break
                    t = d[s.pos]
                    s.pos += 1
                    c = s._var()
                    if c is None or not (0 <= c <= lim - s.pos):
                        break
                    if t == 0x2F:
                        break
                    if t == 0x51:
                        if c != 3:
                            break
                        tempo = (d[s.pos] << 16) | (d[s.pos + 1] << 8) | d[s.pos + 2]
                        s.tempo = tempo
                        s.pos += 3
                        return (s.ts, b"", tempo)
                    s.pos += c
                    continue
                else:
                    break
            else:
                break
            if s.pos + n > lim:
                break
            send += d[s.pos : s.pos + n]
            s.pos += n
            return (s.ts, send, s.tempo)
        s.pos = lim
        return None


def _enc_var(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(out))


def midi_to_mids(data):
    if len(data) < 14 or data[:4] != b"MThd":
        raise ValueError("not a MIDI file")
    hlen = struct.unpack(">I", data[4:8])[0]
    fmt, ntrk, div = struct.unpack(">hHh", data[8:14])
    if hlen + 8 > len(data) or hlen < 6:
        raise ValueError("bad MIDI header")
    tracks = []
    fp = hlen + 8
    while fp < len(data):
        if fp + 8 > len(data):
            raise ValueError("truncated MIDI")
        sig, cb = data[fp : fp + 4], struct.unpack(">I", data[fp + 4 : fp + 8])[0]
        fp += 8
        if cb > len(data) - fp:
            raise ValueError("bad chunk length")
        if sig != b"MTrk" or cb == 0:
            fp += cb
            continue
        t = _Track(data[fp : fp + cb])
        fp += cb
        t.cur = t.next()
        if t.cur is not None:
            tracks.append(t)
            if fmt not in (1, 2):
                break
    if not tracks:
        raise ValueError("no usable track")
    if div < 0:
        fps = (-((div >> 8) & 0xFF)) & 0xFF
        tick = div & 0xFF
        if fps == 29:
            fps = 30
        if tick <= 0 or fps <= 0:
            raise ValueError("bad SMPTE division")
        num, den, smpte = 1000, fps * tick, True
    else:
        if div == 0:
            raise ValueError("bad division")
        num, den, smpte = 500, div, False
    fseq = len(tracks) == 1 or fmt == 2
    tsTempo = tsRaw = tsLast = 0
    out = bytearray()
    i = 0
    while True:
        if not fseq:
            i = min(range(len(tracks)), key=lambda k: (tracks[k].cur[0], k))
        tr = tracks[i]
        ts0, send, tempo = tr.cur
        ts = (
            ((ts0 - tsRaw) * num) // den + tsTempo
            if ts0 >= tsRaw
            else -(((tsRaw - ts0) * num) // den) + tsTempo
        )
        if not send:
            if not smpte:
                n2, d2 = tempo, 1000 * div
                if n2 * den != num * d2:
                    num, den = n2, d2
                    tsRaw = ts0
                    tsTempo = ts
        else:
            out += _enc_var((ts - tsLast) & 0xFFFFFFFF) + send
        tsLast = ts
        nxt = tr.next()
        if nxt is not None:
            tr.cur = nxt
        else:
            del tracks[i]
            if not tracks:
                break
            if fseq:
                tsTempo = tsLast
                tsRaw = 0
                i = 0
    return bytes(out)
