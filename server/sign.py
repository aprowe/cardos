"""The server's signing key, so a device can tell what the server made.

Jar Factory's items are the first use: a device accepts an item from the
network only with the server's signature on it, so an item cannot be
forged or copied in transit. Anything later that must be "from the
server" signs the same way.

ECDSA on P-256 over SHA-256, with the `cryptography` package. The key is
made on first use into CARDOS_STATE/sign_key.pem (PKCS#8, unencrypted,
0600) and kept: a new key orphans every signature the devices hold, so
never delete it casually.

    sign(data) -> 64 bytes: r || s, each 32 bytes big-endian (not DER --
                  a device checks raw numbers, with no ASN.1 reader)
    verify(data, sig, pub=None) -> True / False
    public_raw() -> 65 bytes: 0x04 || X || Y, the uncompressed point

    GET /sign/pubkey -> the 65 bytes as 130 lowercase hex digits and "\\n".
                        Open: a public key is public; a device pins it.

There is no route that signs bytes for a client, and there must never be:
only server code decides what the server vouches for.
"""
import os
import threading

from . import dash

_lock = threading.Lock()
_keys = {}                      # path -> private key


def key_path():
    return os.path.join(dash.state_dir(), "sign_key.pem")


def _load_or_make():
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    path = os.path.abspath(key_path())
    with _lock:
        k = _keys.get(path)
        if k is not None:
            return k
        try:
            with open(path, "rb") as f:
                k = serialization.load_pem_private_key(f.read(), password=None)
        except FileNotFoundError:
            k = ec.generate_private_key(ec.SECP256R1())
            pem = k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption())
            os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
            # O_EXCL: two servers starting at once must not each write a key
            try:
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            except FileExistsError:            # the other one won: use theirs
                with open(path, "rb") as f:
                    k = serialization.load_pem_private_key(f.read(), password=None)
            else:
                with os.fdopen(fd, "wb") as f:
                    f.write(pem)
                    f.flush()
                    os.fsync(f.fileno())
        if not isinstance(getattr(k, "curve", None), ec.SECP256R1):
            raise ValueError("%s is not a P-256 key" % path)
        _keys[path] = k
        return k


def sign(data):
    """64 bytes, r || s, over SHA-256 of `data`."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
    der = _load_or_make().sign(bytes(data), ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def public_raw():
    from cryptography.hazmat.primitives import serialization
    return _load_or_make().public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)


def verify(data, sig, pub=None):
    """Is `sig` (64 raw bytes) the signature of `data` by `pub` (65 raw
    bytes; the server's own key if None)?"""
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
    sig = bytes(sig)
    if len(sig) != 64:
        return False
    if pub is None:
        key = _load_or_make().public_key()
    else:
        try:
            key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), bytes(pub))
        except ValueError:
            return False
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    try:
        key.verify(der, bytes(data), ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def forget_cached():
    """For the tests: the next use reads the file again."""
    with _lock:
        _keys.clear()


def get_pubkey(h, path, args):
    """the server's signing key, for a device to pin (hex)"""
    try:
        h.text(public_raw().hex() + "\n")
    except ImportError:
        h.text("error this server has no cryptography package\n", 503)


ROUTES = [
    ("GET", "/sign/pubkey", get_pubkey, "open"),
]
