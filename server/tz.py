"""/tz: where the device is, as the POSIX rule its `env TZ` needs.

The device cannot do this itself. A geolocation service answers with an IANA
name ("America/Los_Angeles"); the C library on the device only understands a
POSIX rule ("PST8PDT,M3.2.0,M11.1.0"), and the database that maps one to the
other is hundreds of kilobytes it does not have. Asking over HTTPS would also
cost a TLS handshake -- the first attempt did it on a 4 KB background stack
and restarted the device every boot (2026-09-23).

Here, both halves are easy. The caller's address is on the request (nginx
passes it as X-Real-IP), and every zone file in the tz database ends with the
very rule the device needs: a TZif v2+ file carries it as its last line, for
the times past its table. So: address -> zone name -> the file's last line.

A caller on a private network -- the device talking to a laptop on the same
LAN -- has no location of its own to look up, so the server's own public
address stands in for it: same building, same zone.

    GET /tz   ->  "tz PST8PDT,M3.2.0,M11.1.0\nzone America/Los_Angeles\n"
              or  "error <why>\n"
"""
import ipaddress
import json
import os
import sys
import threading
import urllib.request
import zoneinfo

# Keyless and HTTPS; about a thousand lookups a day for free, and the device
# asks once, ever, unless TZ is cleared.
GEO_URL = "https://ipapi.co/%sjson/"

CACHE = {}
_lock = threading.Lock()


def public_ip(ip):
    """The address to look up, or None for "wherever this server is"."""
    try:
        a = ipaddress.ip_address((ip or "").strip())
    except ValueError:
        return None
    if a.is_private or a.is_loopback or a.is_link_local:
        return None
    return str(a)


def zone_for_ip(ip):
    """The IANA zone name for an address, or for this server with None. A
    stranger's server, so it is allowed to fail: None."""
    url = GEO_URL % (ip + "/" if ip else "")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "cardos-server"})
        with urllib.request.urlopen(req, timeout=8) as r:
            return json.loads(r.read().decode("utf-8", "replace")).get("timezone")
    except Exception as e:                             # noqa: BLE001 - reported
        sys.stderr.write("tz: lookup of %s failed: %s\n" % (ip or "this server", e))
        return None


def _zone_file(name):
    """The TZif bytes for a zone name, from the system database or Python's
    tzdata package (Windows has no /usr/share/zoneinfo)."""
    parts = name.split("/")
    if not name or any(p in ("", ".", "..") or not all(c.isalnum() or c in "_-+"
                                                       for c in p) for p in parts):
        return None
    for root in list(zoneinfo.TZPATH):
        path = os.path.join(root, *parts)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                return f.read()
    try:
        from importlib import resources
        pkg = ".".join(["tzdata", "zoneinfo"] + parts[:-1])
        return resources.files(pkg).joinpath(parts[-1]).read_bytes()
    except Exception:                                  # noqa: BLE001
        return None


def posix_rule(name):
    """The POSIX TZ rule for an IANA zone name, or None. It is the footer of a
    version 2+ TZif file: the text between its last two newlines."""
    data = _zone_file(name or "")
    if not data or not data.startswith(b"TZif") or data[4:5] not in (b"2", b"3", b"4"):
        return None
    if not data.endswith(b"\n"):
        return None
    footer = data[data.rstrip(b"\n").rfind(b"\n") + 1:].strip()
    try:
        rule = footer.decode("ascii")
    except UnicodeDecodeError:
        return None
    return rule or None


def lookup(ip):
    """(rule, zone) for the caller at `ip`, or (None, why)."""
    key = public_ip(ip)
    with _lock:
        if key in CACHE:
            return CACHE[key]
    zone = zone_for_ip(key)
    if not zone:
        return None, "could not tell where %s is" % (key or "this server")
    rule = posix_rule(zone)
    if not rule:
        return None, "no rule for zone %s" % zone
    with _lock:
        CACHE[key] = (rule, zone)
    return rule, zone


def get_tz(h, path, args):
    """the caller's time zone, as a POSIX TZ rule"""
    ip = h.headers.get("X-Real-IP") or h.client_address[0]
    rule, zone = lookup(ip)
    if not rule:
        h.text("error %s\n" % zone)
        return
    sys.stderr.write("tz: %s -> %s (%s)\n" % (ip, zone, rule))
    h.text("tz %s\nzone %s\n" % (rule, zone))


ROUTES = [("GET", "/tz", get_tz)]
