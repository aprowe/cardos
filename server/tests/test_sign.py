"""server/sign.py: the server's P-256 key, raw signatures, the pubkey route.

    python -m server.tests.test_sign
"""
import hashlib
import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from http.server import ThreadingHTTPServer
from server import app, sign
from server import chat as chatmod


class Sign(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        os.environ["CARDOS_STATE"] = self.dir
        sign.forget_cached()

    def tearDown(self):
        sign.forget_cached()
        os.environ.pop("CARDOS_STATE", None)
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_round_trip(self):
        data = b"\x01item No. 0042: Snail"
        sig = sign.sign(data)
        self.assertEqual(len(sig), 64)
        self.assertTrue(sign.verify(data, sig))
        self.assertFalse(sign.verify(data + b"!", sig))
        bent = bytes([sig[0] ^ 1]) + sig[1:]
        self.assertFalse(sign.verify(data, bent))
        self.assertFalse(sign.verify(data, sig[:63]))

    def test_raw_signature_verifies_with_the_raw_pubkey(self):
        # as a device would: only the 65-byte point and r||s, no PEM, no DER
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
        pub = sign.public_raw()
        self.assertEqual((len(pub), pub[0]), (65, 4))
        data = b"gift from sam"
        sig = sign.sign(data)
        key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), pub)
        r, s = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")
        key.verify(encode_dss_signature(r, s), data, ec.ECDSA(hashes.SHA256()))   # raises if not
        # and sign.verify agrees given the raw key, and refuses another's
        self.assertTrue(sign.verify(data, sig, pub))
        other = ec.generate_private_key(ec.SECP256R1()).public_key().public_bytes(
            serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        self.assertFalse(sign.verify(data, sig, other))
        self.assertFalse(sign.verify(data, sig, b"\x04" + b"\x00" * 64))
        # what a device checks: ECDSA over the SHA-256 digest of the bytes
        from cryptography.hazmat.primitives.asymmetric.utils import Prehashed
        key.verify(encode_dss_signature(r, s), hashlib.sha256(data).digest(),
                   ec.ECDSA(Prehashed(hashes.SHA256())))

    def test_the_key_is_kept(self):
        pub = sign.public_raw()
        path = sign.key_path()
        self.assertTrue(os.path.exists(path))
        if os.name == "posix":
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
        sig = sign.sign(b"x")
        sign.forget_cached()                       # as a restart would
        self.assertEqual(sign.public_raw(), pub)
        self.assertTrue(sign.verify(b"x", sig))

    def test_pubkey_route_and_no_signing_route(self):
        app.Handler.chat = chatmod.ChatService(claude="stub", token="tok")
        srv = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        base = "http://127.0.0.1:%d" % srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            body = urllib.request.urlopen(base + "/sign/pubkey", timeout=20).read()
            self.assertEqual(body, (sign.public_raw().hex() + "\n").encode())
        finally:
            srv.shutdown()
            srv.server_close()
        for method, pattern, fn, auth in app.ALL_ROUTES:
            if pattern.startswith("/sign"):
                self.assertEqual((method, pattern), ("GET", "/sign/pubkey"))


if __name__ == "__main__":
    unittest.main()
