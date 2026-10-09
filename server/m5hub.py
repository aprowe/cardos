"""LauncherHub, M5Launcher's firmware catalog, for the device's Hub app.

api.launcherhub.net lists every firmware published for a device (695 for
the Cardputer, 2026-10-08) and m5burner-cdn.m5stack.com holds the files.
The files are mostly whole-flash images -- bootloader, partition table, app,
data -- and each version says where its app is inside (`ao`, `as`). CardOS
boots a guest's app image only (kernel/app/launcher.c, into the OTA slot
not running), so the device fetches just that range: 490 KB of a 4 MB file.

All here, over HTTPS and JSON, so the device needs neither. Lines out:

    GET /m5hub/list?page=N&order=name|date|star&q=TEXT
        -> "ok PAGE PAGES TOTAL", then fid <tab> name <tab> author <tab> stars
    GET /m5hub/info?fid=F
        -> "ok NAME", then version <tab> date <tab> file <tab> app offset
           <tab> app size <tab> 1 if it also wants a data partition, newest first
    GET /m5hub/get?file=X&ao=N&as=N
        -> the app image's bytes, with their length
"""
import json
import re
import sys
import time
import urllib.parse
import urllib.request

from . import jobs

API = "https://api.launcherhub.net/firmwares"
CDN = "https://m5burner-cdn.m5stack.com/firmware/"
CATEGORY = "cardputer"
TIMEOUT = 30
APP_MAX = 0x240000            # an OTA slot, partitions.csv: a bigger app cannot boot
CACHE_S = 600

_FILE = re.compile(r"^[A-Za-z0-9._-]{1,80}\.bin$")
_FID = re.compile(r"^[0-9a-f]{8,64}$")
_cache = jobs.LRU(64)            # url -> (when, the catalog's answer)


def _flat(s, n=60):
    """One line of ASCII: the device's font has nothing past 0x7E, and a
    catalog full of emoji would draw as boxes."""
    s = str(s or "").encode("ascii", "ignore").decode()
    return " ".join(str(s or "").replace("\t", " ").split())[:n]


def _json(url, opener=urllib.request.urlopen):
    hit = _cache.get(url)
    if hit and time.time() - hit[0] < CACHE_S:
        return hit[1]
    req = urllib.request.Request(url, headers={"User-Agent": "CardOS"})
    with opener(req, timeout=TIMEOUT) as r:
        d = json.loads(r.read())
    _cache[url] = (time.time(), d)
    return d


ORDERS = {"name": "name", "date": "date", "star": "name"}


# The device's pages are smaller than the catalog's 100: a reply must fit
# the kernel's 8 KB HTTP buffer, and 40 lines of at most ~95 bytes does.
PER = 40


def _upstream(page, order, q, opener):
    params = {"category": CATEGORY, "order_by": ORDERS.get(order, "name")}
    if page > 1:
        params["page"] = str(page)
    if q:
        params["q"] = q
    if order == "star":
        params["star"] = "1"
    return _json(API + "?" + urllib.parse.urlencode(params), opener)


def list_lines(page=1, order="name", q="", opener=urllib.request.urlopen):
    first = _upstream(1, order, q, opener)
    size = max(1, int(first.get("page_size") or 100))
    total = int(first.get("total") or 0)
    pages = max(1, (total + PER - 1) // PER)
    page = min(max(1, page), pages)
    start = (page - 1) * PER
    items, up = [], start // size + 1
    while len(items) < (start % size) + PER and (up - 1) * size < total:
        d = first if up == 1 else _upstream(up, order, q, opener)
        got = d.get("items") or []
        items += got
        if not got:
            break
        up += 1
    items = items[start % size:start % size + PER]
    out = ["ok %d %d %d" % (page, pages, total)]
    for x in items:
        fid = str(x.get("fid") or "")
        if _FID.match(fid):
            out.append("%s\t%s\t%s\t%d" % (fid, _flat(x.get("name"), 40), _flat(x.get("author"), 16),
                                           int(x.get("star") or 0)))
    return "\n".join(out) + "\n"


def info_lines(fid, opener=urllib.request.urlopen):
    d = _json(API + "?" + urllib.parse.urlencode({"fid": fid}), opener)
    out = ["ok " + _flat(d.get("name"))]
    vs = sorted(d.get("versions") or [], key=lambda v: str(v.get("published_at") or ""),
                reverse=True)
    for v in vs:
        f = str(v.get("file") or "")
        ao, size = int(v.get("ao") or 0), int(v.get("as") or 0)
        if not _FILE.match(f) or size <= 0:
            continue
        data = 1 if (v.get("s") or v.get("f") or v.get("f2")) else 0
        out.append("%s\t%s\t%s\t%d\t%d\t%d" % (_flat(v.get("version"), 20),
                                               _flat(v.get("published_at"), 10), f, ao, size, data))
    return "\n".join(out) + "\n"


def fetch_app(file, ao, size, opener=urllib.request.urlopen):
    """The app image's bytes: a range of the file, or the whole of a file
    that is the app (ao 0). Checked to start like one."""
    req = urllib.request.Request(CDN + file, headers={
        "User-Agent": "CardOS", "Range": "bytes=%d-%d" % (ao, ao + size - 1)})
    with opener(req, timeout=120) as r:
        if getattr(r, "status", 206) == 200:
            data = r.read(ao + size)[ao:]        # the range was ignored: the whole file came
        else:
            data = r.read(size)
    if len(data) != size:
        raise ValueError("got %d bytes of %d" % (len(data), size))
    if data[:1] != b"\xe9":
        raise ValueError("not an app image at offset %d" % ao)
    return data


# ---- routes ---------------------------------------------------------------

def _arg(args, k, default=""):
    return (args.get(k) or [default])[0]


def get_list(h, path, args):
    """a page of the catalog"""
    try:
        page = max(1, int(_arg(args, "page", "1")))
        h.text(list_lines(page, _arg(args, "order", "name"), _arg(args, "q")[:40]))
    except (OSError, ValueError) as e:
        h.text("error the catalog did not answer: %s\n" % e, 502)


def get_info(h, path, args):
    """a firmware's versions"""
    fid = _arg(args, "fid")
    if not _FID.match(fid):
        h.text("error bad fid\n", 400)
        return
    try:
        h.text(info_lines(fid))
    except (OSError, ValueError) as e:
        h.text("error the catalog did not answer: %s\n" % e, 502)


def get_app(h, path, args):
    """a version's app image"""
    f = _arg(args, "file")
    try:
        ao, size = int(_arg(args, "ao", "0")), int(_arg(args, "as", "0"))
    except ValueError:
        ao, size = -1, 0
    if not _FILE.match(f) or ao < 0 or not 0 < size <= APP_MAX:
        h.text("error that is not something the device can boot\n", 400)
        return
    try:
        data = fetch_app(f, ao, size)
    except (OSError, ValueError) as e:
        sys.stderr.write("m5hub: %s: %s\n" % (f, e))
        h.text("error %s\n" % e, 502)
        return
    h.send_response(200)
    h.send_header("Content-Type", "application/octet-stream")
    h.send_header("Content-Length", str(len(data)))
    h.end_headers()
    h.wfile.write(data)


ROUTES = [
    ("GET", "/m5hub/list", get_list, "token"),
    ("GET", "/m5hub/info", get_info, "token"),
    ("GET", "/m5hub/get", get_app, "token"),
]
