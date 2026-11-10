"""bcm overlay v78: the "Rockbox Plus" start-up voice for the iPod's click speaker (piezo).

A small formant synthesizer (Klatt-style: a cascade of five resonators fed by a glottal pulse with breath noise for the
voiced sounds, filtered noise for s, bursts and aspiration), no text-to-speech engine needed.  It speaks a fixed list of
phonemes.  The result is still robotic, like a 1980s speech chip; the piezo makes it rougher.  The output is shaped for
the element (bass cut at 350 Hz -- a piezo disc cannot move air down there --, +6 dB around 2.8 kHz where it is loud and
where speech is understood, then compressed) so it uses the little loudness the element has on what matters.

A recording of your own voice works better: put it on the iPod as /.rockbox/piezo/boot.wav (any WAV: 8 / 16-bit PCM,
mono or stereo, any rate, the first 3 s) and Rockbox plays that instead (the same bass cut is applied to it).

    python3 piezo_voice.py [--wav preview.wav] [--raw-wav unshaped.wav] [--header apps/bcm_piezo_voice.h] [--rate 16000]

Python 3 standard library only.
"""
import argparse, math, random, struct, wave

RATE = 16000

# phoneme targets: F1 F2 F3 (Hz), B1 B2 B3, voicing, aspiration (noise through the formants), frication (noise outside
# them: amplitude, centre, bandwidth; for the stops a short burst at the release)
P = {
    'r':  dict(f=(420, 1150, 1550), b=(70, 90, 110), av=0.80),
    'aa': dict(f=(740, 1100, 2450), b=(90, 90, 140), av=1.00),
    'ah': dict(f=(640, 1200, 2400), b=(80, 90, 130), av=1.00),
    'l':  dict(f=(370, 1000, 2750), b=(60, 120, 160), av=0.80),
    'cl': dict(f=(300, 1100, 2400), b=(90, 120, 160), av=0.00),
    'vb': dict(f=(200, 1000, 2400), b=(60, 150, 200), av=0.10),     # voice bar (b closure)
    'kb': dict(f=(450, 1900, 2500), b=(90, 120, 160), av=0.00, fr=(0.9, 2100, 1100)),   # k release
    'bb': dict(f=(300, 900, 2200), b=(90, 120, 160), av=0.20, fr=(0.35, 1000, 900)),     # b release
    'pb': dict(f=(400, 1000, 2300), b=(90, 120, 160), av=0.00, fr=(0.8, 1200, 1500)),    # p release
    'hh': dict(f=(620, 1200, 2400), b=(150, 150, 200), av=0.00, asp=0.55),               # aspiration
    's':  dict(f=(500, 1500, 2500), b=(90, 120, 160), av=0.00, fr=(1.0, 5300, 2200)),
}
F45 = ((3300, 250), (3850, 300))
# (phoneme, ms, pitch at its end): "ROCK-box PLUS", stress on rock and plus
SEQ = [
    ('cl', 25, 118),
    ('r', 80, 128), ('aa', 175, 138),
    ('cl', 70, 120), ('kb', 18, 118),
    ('vb', 45, 112), ('bb', 12, 112), ('aa', 145, 102),
    ('cl', 60, 100), ('kb', 18, 100), ('s', 110, 100),
    ('cl', 70, 116), ('pb', 12, 124), ('hh', 50, 132),
    ('l', 75, 150), ('ah', 170, 112), ('s', 190, 96),
    ('cl', 40, 96),
]
TRANS_MS = 35


class Reso:
    """two-pole resonator (Klatt), unity gain at DC"""
    def __init__(self):
        self.y1 = self.y2 = 0.0

    def run(self, x, f, bw, rate):
        t = 1.0 / rate
        c = -math.exp(-2 * math.pi * bw * t)
        b = 2 * math.exp(-math.pi * bw * t) * math.cos(2 * math.pi * f * t)
        a = 1 - b - c
        y = a * x + b * self.y1 + c * self.y2
        self.y2, self.y1 = self.y1, y
        return y


class Biquad:
    def __init__(self, b0, b1, b2, a1, a2):
        self.c = (b0, b1, b2, a1, a2)
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0

    def run(self, x):
        b0, b1, b2, a1, a2 = self.c
        y = b0 * x + b1 * self.x1 + b2 * self.x2 - a1 * self.y1 - a2 * self.y2
        self.x2, self.x1, self.y2, self.y1 = self.x1, x, self.y1, y
        return y

    @staticmethod
    def highpass(f, rate, q=0.707):
        w = 2 * math.pi * f / rate
        al = math.sin(w) / (2 * q)
        cw = math.cos(w)
        a0 = 1 + al
        return Biquad((1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0, -2 * cw / a0, (1 - al) / a0)

    @staticmethod
    def peak(f, rate, db, q=1.0):
        A = 10 ** (db / 40)
        w = 2 * math.pi * f / rate
        al = math.sin(w) / (2 * q)
        cw = math.cos(w)
        a0 = 1 + al / A
        return Biquad((1 + al * A) / a0, -2 * cw / a0, (1 - al * A) / a0, -2 * cw / a0, (1 - al / A) / a0)


def smooth(k):
    return 0.5 - 0.5 * math.cos(math.pi * max(0.0, min(1.0, k)))


def synth(rate=RATE, seed=7):
    rnd = random.Random(seed)
    segs, t = [], 0.0
    p0 = SEQ[0][2]
    for ph, ms, pe in SEQ:
        segs.append((t, t + ms / 1000.0, ph, p0, pe))
        t += ms / 1000.0
        p0 = pe
    n = int(t * rate)
    cas = [Reso() for _ in range(5)]
    fric = Reso()
    tilt = 0.0
    phase = 0.0
    lip = 0.0
    prev_noise = 0.0
    tr = TRANS_MS / 1000.0
    si = 0
    voiced, noise_out = [], []
    for i in range(n):
        tt = i / rate
        while si + 1 < len(segs) and tt >= segs[si][1]:
            si += 1
        s0, s1, ph, pa, pb = segs[si]
        cur = P[ph]
        f, bw, av = list(cur['f']), list(cur['b']), cur.get('av', 0.0)
        if si > 0 and tt - s0 < tr:
            k = smooth((tt - s0) / tr)
            prv = P[segs[si - 1][2]]
            f = [x + (y - x) * k for x, y in zip(prv['f'], cur['f'])]
            bw = [x + (y - x) * k for x, y in zip(prv['b'], cur['b'])]
            av = prv.get('av', 0.0) + (av - prv.get('av', 0.0)) * min(1.0, k * 1.4)
        # pitch: along the segment, a little vibrato and jitter
        f0 = pa + (pb - pa) * (tt - s0) / max(1e-6, s1 - s0)
        f0 *= 1.0 + 0.006 * math.sin(2 * math.pi * 5.1 * tt) + 0.003 * rnd.uniform(-1, 1)
        phase += f0 / rate
        if phase >= 1.0:
            phase -= 1.0
        op = 0.62                                   # open quotient
        if phase < op * 0.7:
            g = 0.5 * (1 - math.cos(math.pi * phase / (op * 0.7)))
        elif phase < op:
            g = math.cos(0.5 * math.pi * (phase - op * 0.7) / (op * 0.3))
        else:
            g = 0.0
        tilt += 0.35 * (g - tilt)                   # spectral tilt: a softer, less buzzy voice
        noise = rnd.uniform(-1, 1)
        breath = 0.04 * noise * (1.0 if phase < op else 0.3)
        x = av * (tilt + breath) + cur.get('asp', 0.0) * noise * 0.5
        for r, ff, b in zip(cas, f + [F45[0][0], F45[1][0]], bw + [F45[0][1], F45[1][1]]):
            x = r.run(x, min(ff, rate * 0.45), b, rate)
        y = x - lip                                 # lip radiation
        lip = x
        fy = 0.0
        fr = cur.get('fr')
        if fr:
            amp, fc, fbw = fr
            if ph == 's':
                env = min(1.0, (tt - s0) / 0.015, (s1 - tt) / 0.02)
            else:
                env = math.exp(-(tt - s0) / 0.006)  # a burst: sharp onset, ~6 ms decay
            fy = amp * env * fric.run(noise - prev_noise, min(fc, rate * 0.45), fbw, rate)
        prev_noise = noise
        voiced.append(y)
        noise_out.append(fy)
    pv = max(abs(v) for v in voiced) or 1.0
    pf = max(abs(v) for v in noise_out) or 1.0
    return [v / pv + 0.45 * w / pf for v, w in zip(voiced, noise_out)]


def shape_for_piezo(x, rate):
    """bass cut, presence lift, normalise, soft compression"""
    hp1, hp2 = Biquad.highpass(350, rate), Biquad.highpass(350, rate)
    pk = Biquad.peak(2800, rate, 6.0, 1.0)
    y = [pk.run(hp2.run(hp1.run(v))) for v in x]
    m = max(abs(v) for v in y) or 1.0
    return [math.tanh(1.8 * v / m) / math.tanh(1.8) for v in y]


def to_u8(samples):
    return bytes(max(0, min(255, int(round(128 + 127 * v)))) for v in samples)


def write_wav(path, samples, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b''.join(struct.pack('<h', int(max(-1, min(1, v)) * 32000)) for v in samples))


def write_header(path, u8, rate):
    lines = ['/* bcm overlay v78: the click speaker start-up voice "Rockbox Plus" -- made by tools/bcm/piezo_voice.py',
             ' * (formant synthesis, shaped for the piezo), 8-bit unsigned, %d Hz, %d samples.  Do not edit: run the'
             ' script. */' % (rate, len(u8)),
             '#define BCM_PIEZO_VOICE_RATE %d' % rate,
             'static const unsigned char bcm_piezo_voice[%d] = {' % len(u8)]
    for i in range(0, len(u8), 20):
        lines.append('    ' + ','.join('%d' % b for b in u8[i:i + 20]) + ',')
    lines.append('};')
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    ap = argparse.ArgumentParser(description='make the "Rockbox Plus" piezo voice')
    ap.add_argument('--wav', help='a WAV to listen to on a computer (as the iPod gets it)')
    ap.add_argument('--raw-wav', help='the same before the piezo shaping (fuller sound)')
    ap.add_argument('--header', help='the C header for apps/')
    ap.add_argument('--rate', type=int, default=RATE)
    a = ap.parse_args()
    raw = synth(a.rate)
    s = shape_for_piezo(raw, a.rate)
    if a.raw_wav:
        m = max(abs(v) for v in raw) or 1.0
        write_wav(a.raw_wav, [v / m * 0.9 for v in raw], a.rate)
    if a.wav:
        write_wav(a.wav, s, a.rate)
    if a.header:
        write_header(a.header, to_u8(s), a.rate)
    print('%d samples, %.2f s' % (len(s), len(s) / a.rate))


if __name__ == '__main__':
    main()
