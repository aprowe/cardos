# Photos

_Moved from CLAUDE.md on 2026-10-27._

**Photos: drag pictures onto the dashboard, see and print them on the
device** (2026-10-02). The dashboard's Photos page takes JPG, PNG, GIF,
WebP; `server/photos.py` keeps each with a screen copy (RGB565 `.img`,
fitted to 240x135 with bars -- `server/images.py`) and a print copy (the
picture dithered to the printer's 384 dots as printdoc `%%` lines, rows
merged, runs or raw per row). The device never decodes JPEG or PNG: no
decoder, and flash is short. `apps/photo.c` (Photos) syncs on open --
the list, then one picture a tick, and what is gone from the server goes
from /pics -- flips with the arrows, slideshows on space (holding the
screen on), prints on p / fn-p by fetching the print copy to /cache and
running `print FILE` through `api->shell`, which now reads the whole file
(up to 48 KB; it stopped at 6). `api->http_download` now sends the
device's token to its own server, as `http` and `http_upload` did.
