"""Writes the SNTP fuzz seeds (core/tests/fuzz-seeds/sntp/): answers to the nonce
fuzz_sntp.c uses, one per check of docs/formats/sntp.md section 4.

Usage: python3 tools/fixtures/sntp_seeds.py core/tests/fuzz-seeds/sntp
"""
import struct, sys, os
out = sys.argv[1]
T = 1792195200 + 2208988800
nonce = bytes([0x5A,0x01,0xC3,0x7E,0x00,0xFF,0x10,0x99])
def pkt(b0=0x24, stratum=2, refid=b'GPS\0', orig=nonce, recv=(T,0), xmit=(T,0x80000000), extra=b''):
    return (bytes([b0, stratum, 6, 0xEC]) + struct.pack('>II', 0, 0) + refid + struct.pack('>II', T-30, 0)
            + orig + struct.pack('>II', *recv) + struct.pack('>II', *xmit) + extra)
seeds = {
  'good': pkt(),
  'good-v3-mac': pkt(b0=0x1C, extra=b'\0\0\0\1' + b'\xAB'*16),
  'kiss-rate': pkt(stratum=0, refid=b'RATE'),
  'unsynced-li3': pkt(b0=0xE4),
  'stratum16': pkt(stratum=16),
  'era1': pkt(xmit=(4102444800 + 2208988800 - 2**32, 0)),
  'no-transmit': pkt(xmit=(0,0)),
  'short': pkt()[:47],
  'request': bytes([0x23]) + bytes(39) + nonce,
}
for k, v in seeds.items():
    open(os.path.join(out, k + '.seed'), 'wb').write(v)
