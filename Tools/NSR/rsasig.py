"""rsasig.py: RSASSA-PKCS1-v1_5 / SHA-256 signature *verification*, standard library only.

Shared by two verifiers that must not grow a third-party dependency:
  - services/ratings/access.py checks Cloudflare Access JWTs (RS256);
  - tools/nsr_content.py checks signed content-update manifests inside the
    friends' launcher (NFLStreetRollback.exe is a PyInstaller build whose
    pip installs are kept to pyinstaller + pywebview; see docs/content-updates.md
    "Why RSA and not Ed25519").

Verification is one modular exponentiation plus a constant-time comparison
against the exact expected encoding (RFC 8017 section 8.2.2). Comparing the
whole encoded message, never parsing it, avoids the classic signature-forgery
parsing bugs (Bleichenbacher 2006 style "garbage after the hash"). No
private-key math happens here, so constant-time arithmetic is not needed.
Signing lives server-side only (services/content/content_store.py, via the
`cryptography` package).
"""
from __future__ import annotations

import hashlib
import hmac

# DER DigestInfo prefix for SHA-256 (RFC 8017 section 9.2, note 1).
SHA256_PREFIX = bytes.fromhex("3031300d060960864801650304020105000420")


def rsa_pkcs1v15_sha256_verify(n: int, e: int, message: bytes, signature: bytes) -> bool:
    """RFC 8017 RSASSA-PKCS1-v1_5-VERIFY with SHA-256, by full-encoding comparison."""
    k = (n.bit_length() + 7) // 8
    if len(signature) != k:
        return False
    s = int.from_bytes(signature, "big")
    if s >= n:
        return False
    em = pow(s, e, n).to_bytes(k, "big")
    t = SHA256_PREFIX + hashlib.sha256(message).digest()
    if k < len(t) + 11:
        return False
    expected = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    return hmac.compare_digest(em, expected)
