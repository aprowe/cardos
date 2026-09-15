#!/usr/bin/env python3
"""Exercise the device's WebDAV share from the PC.

    python tools/test_share.py 192.168.1.23
    python tools/test_share.py 192.168.1.23 --keep

Every method the server claims, a 1 MB round trip compared byte for byte,
a traversal attempt that must be refused, and the transfer rates -- which
belong in the commit message, per the working agreements.

--keep skips the final DELETE of the test folder, so it can be looked at
in Explorer afterwards.
"""
import http.client
import os
import re
import sys
import time

MB = 1024 * 1024
TEST_DIR = "/sharetest"


class Dav:
    def __init__(self, host):
        self.host = host

    def req(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection(self.host, 80, timeout=30)
        h = dict(headers or {})
        if body is not None:
            h["Content-Length"] = str(len(body))
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, dict(r.getheaders()), data


def check(cond, what, status=None):
    print(("  ok   " if cond else "  FAIL ") + what +
          ("" if cond or status is None else " (status %r)" % (status,)))
    if not cond:
        check.fails += 1


check.fails = 0


def hrefs(body):
    """Last path component of every <D:href> in a PROPFIND response body."""
    names = []
    for m in re.finditer(rb"<D:href>([^<]*)</D:href>", body):
        name = m.group(1).decode("utf-8", "replace").rstrip("/")
        names.append(name.rsplit("/", 1)[-1])
    return names


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--keep"):
        sys.exit(__doc__)
    keep = len(sys.argv) == 3
    host = sys.argv[1]
    print("testing share at http://%s/ (keep=%s)" % (host, keep))
    d = Dav(host)

    try:
        st, h, _ = d.req("OPTIONS", "/")
    except OSError as e:
        sys.exit("could not reach http://%s/ -- %s" % (host, e))
    check(st == 200 and "1,2" in h.get("DAV", ""), "OPTIONS advertises DAV: 1,2", st)
    check("MS-Author-Via" in h, "OPTIONS has MS-Author-Via")

    st, _, body = d.req("PROPFIND", "/", headers={"Depth": "0"})
    check(st == 207 and b"<D:collection/>" in body, "PROPFIND / depth 0 is a collection", st)
    st, _, body = d.req("PROPFIND", "/desktop", headers={"Depth": "1"})
    check(st == 207 and body.count(b"<D:response>") > 1, "PROPFIND /desktop depth 1 lists children", st)
    st, _, _ = d.req("PROPFIND", "/", headers={"Depth": "infinity"})
    check(st == 403, "Depth: infinity is refused", st)
    st, _, _ = d.req("PROPFIND", "/")
    check(st == 403, "PROPFIND with no Depth header is refused", st)

    d.req("DELETE", TEST_DIR)                       # from a previous run, if any
    st, _, _ = d.req("MKCOL", TEST_DIR)
    check(st == 201, "MKCOL creates a folder", st)
    st, _, _ = d.req("MKCOL", TEST_DIR)
    check(st == 405, "MKCOL on an existing folder is 405", st)
    st, _, _ = d.req("MKCOL", "/nope/deeper")
    check(st == 409, "MKCOL without a parent is 409", st)
    st, h, _ = d.req("MKCOL", TEST_DIR + "/withbody", body=b"<x/>")
    check(st == 415, "MKCOL with a body is 415", st)

    blob = os.urandom(MB)
    t = time.time()
    st, _, _ = d.req("PUT", TEST_DIR + "/one.bin", body=blob)
    up = MB / (time.time() - t) / 1024
    check(st == 201, "PUT creates (%d KB/s up)" % up, st)
    st, _, _ = d.req("PUT", TEST_DIR + "/one.bin", body=blob[:10])
    check(st == 204, "PUT over an existing file is 204", st)
    st, _, _ = d.req("PUT", TEST_DIR + "/one.bin", body=blob)
    check(st == 204, "PUT restores it", st)
    st, _, _ = d.req("PUT", "/nope/deeper.bin", body=b"x")
    check(st == 409, "PUT to a missing parent is 409", st)

    t = time.time()
    st, h, got = d.req("GET", TEST_DIR + "/one.bin")
    down = MB / (time.time() - t) / 1024
    check(st == 200 and got == blob, "GET returns the same bytes (%d KB/s down)" % down, st)
    check(h.get("Content-Length") == str(MB), "GET has Content-Length")
    st, h, got = d.req("HEAD", TEST_DIR + "/one.bin")
    check(st == 200 and got == b"" and h.get("Content-Length") == str(MB), "HEAD has the length, no body", st)

    st, _, _ = d.req("MOVE", TEST_DIR + "/one.bin",
                     headers={"Destination": "http://%s%s/two.bin" % (d.host, TEST_DIR)})
    check(st == 201, "MOVE renames", st)
    st, _, _ = d.req("GET", TEST_DIR + "/one.bin")
    check(st == 404, "the old name is gone", st)
    st, _, _ = d.req("COPY", TEST_DIR + "/two.bin",
                     headers={"Destination": "http://%s%s/three.bin" % (d.host, TEST_DIR)})
    check(st == 201, "COPY copies a file", st)
    st, _, _ = d.req("COPY", TEST_DIR + "/two.bin",
                     headers={"Destination": "http://%s%s/three.bin" % (d.host, TEST_DIR),
                              "Overwrite": "F"})
    check(st == 412, "COPY with Overwrite: F onto an existing file is 412", st)
    st, _, _ = d.req("COPY", TEST_DIR, headers={"Destination": "http://%s/sharetest2" % d.host})
    check(st == 403, "COPY of a folder is the documented 403", st)
    st, _, _ = d.req("PROPFIND", "/sharetest2", headers={"Depth": "0"})
    check(st == 404, "and no target was left behind", st)

    st, _, body = d.req("PROPFIND", TEST_DIR, headers={"Depth": "1"})
    got_names = sorted(n for n in hrefs(body) if n)
    want_names = sorted(["sharetest", "two.bin", "three.bin"])
    check(st == 207 and got_names == want_names,
          "PROPFIND %s depth 1 lists exactly the files created" % TEST_DIR, (st, got_names))

    st, _, body = d.req("PROPPATCH", TEST_DIR + "/two.bin", body=b"<x/>")
    check(st == 207 and b"200 OK" in body, "PROPPATCH says yes", st)
    st, h, body = d.req("LOCK", TEST_DIR + "/two.bin", body=b"<x/>")
    check(st == 200 and "Lock-Token" in h and b"locktoken" in body, "LOCK hands out a token", st)
    st, _, _ = d.req("UNLOCK", TEST_DIR + "/two.bin", headers={"Lock-Token": h.get("Lock-Token", "")})
    check(st == 204, "UNLOCK is 204", st)

    st, _, _ = d.req("GET", "/../etc")
    check(st in (403, 400), "traversal is refused (%d)" % st, st)
    st, _, _ = d.req("GET", "/%2e%2e/x")
    check(st == 403, "encoded traversal is refused", st)
    st, _, _ = d.req("BREW", "/")
    check(st == 405, "an unknown method is 405", st)
    st, h, _ = d.req("BREW", "/")
    check("Allow" in h, "405 carries Allow")
    st, _, _ = d.req("GET", "/does-not-exist")
    check(st == 404, "a missing file is 404", st)
    st, _, _ = d.req("DELETE", "/")
    check(st == 403, "DELETE / is refused", st)

    c = http.client.HTTPConnection(d.host, 80, timeout=30)
    c.putrequest("PUT", TEST_DIR + "/chunked.bin")
    c.putheader("Transfer-Encoding", "chunked")
    c.endheaders()
    try:
        st = c.getresponse().status
    except http.client.HTTPException:
        st = None
    c.close()
    check(st == 411, "a chunked request body is refused", st)

    if keep:
        print("  --keep: leaving %s in place" % TEST_DIR)
    else:
        st, _, _ = d.req("DELETE", TEST_DIR)
        check(st == 204, "DELETE removes the folder and its files", st)
        st, _, _ = d.req("PROPFIND", TEST_DIR, headers={"Depth": "0"})
        check(st == 404, "and it is gone", st)

    print("\n%d failures; up %d KB/s, down %d KB/s" % (check.fails, up, down))
    sys.exit(1 if check.fails else 0)


if __name__ == "__main__":
    main()
