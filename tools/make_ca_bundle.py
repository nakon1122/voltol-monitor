# Small x509 root bundle for WiFiClientSecure::setCACertBundle() (Arduino-ESP32
# 2.0.x format, same as BattLab) with only the roots GitHub's file servers
# (raw.githubusercontent.com) have used or could plausibly move to.
# Format: u16 count, then per cert sorted by subject DER: u16 name_len, u16 key_len, subject, SPKI.
import struct, sys
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import serialization

ROOT = Path(__file__).resolve().parents[1]
WANT = ['USERTrust ECC Certification Authority', 'USERTrust RSA Certification Authority', 'AAA Certificate Services',
        'COMODO Certification Authority', 'Sectigo Public Server Authentication Root E46',
        'Sectigo Public Server Authentication Root R46', 'DigiCert Global Root CA', 'DigiCert Global Root G2',
        'DigiCert Global Root G3', 'DigiCert High Assurance EV Root CA', 'DigiCert TLS ECC P384 Root G5',
        'DigiCert TLS RSA4096 Root G5', 'ISRG Root X1', 'ISRG Root X2', 'GTS Root R1', 'GTS Root R2', 'GTS Root R3',
        'GTS Root R4', 'Baltimore CyberTrust Root']
src = [Path(sys.argv[1] if len(sys.argv) > 1 else '/etc/ssl/certs/ca-certificates.crt'), ROOT / 'assets/legacy_roots.pem']
entries = {}
for f in src:
    for c in x509.load_pem_x509_certificates(f.read_bytes()):
        cn = c.subject.get_attributes_for_oid(x509.NameOID.COMMON_NAME)
        if cn and cn[0].value in WANT:
            entries.setdefault(c.subject.public_bytes(), c.public_key().public_bytes(
                serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo))
items = sorted(entries.items())
out = bytearray(struct.pack('>H', len(items)))
for name, key in items:
    out += struct.pack('>HH', len(name), len(key)) + name + key
lines = [','.join(str(b) for b in out[i:i + 32]) + ',' for i in range(0, len(out), 32)]
(ROOT / 'include/github_ca_bundle.h').write_text(
    '#pragma once\n#include <stdint.h>\n// Root CAs for HTTPS to raw.githubusercontent.com (tools/make_ca_bundle.py). Do not edit.\n'
    f'// {len(items)} roots, {len(out)} bytes\nstatic const uint8_t GITHUB_CA_BUNDLE[] = {{\n' + '\n'.join(lines) + '\n};\n')
print(len(items), 'roots,', len(out), 'bytes')
