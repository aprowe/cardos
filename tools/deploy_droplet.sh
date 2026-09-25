#!/usr/bin/env bash
# Make the droplet build CardOS itself, so Build works with no PC switched on.
# Design: docs/superpowers/specs/2026-09-22-remote-build-design.md.
#
#     bash tools/deploy_droplet.sh setup     once: repo, toolchain, first build, service
#     bash tools/deploy_droplet.sh update    after pushing new master: merge it in, restart
#     bash tools/deploy_droplet.sh pull      bring the droplet's Build commits to this laptop
#     bash tools/deploy_droplet.sh sync      all three -- here, droplet, GitHub -- the same
#     bash tools/deploy_droplet.sh dash [CLIENT.json]
#                                            https://cardos.arowe.net/dash: nginx site,
#                                            certificate, Google Web client into the env
#
# Run from the repository root on the laptop. Uses the root ssh login that
# already exists; adds no keys and no accounts. Everything on the droplet runs
# as the service user `cardos`, except the unit file and the store directory.
#
# Undo `setup`: copy the files in /root/backup-remotebuild-* back to where they
# came from and `systemctl daemon-reload && systemctl restart cardos-proxy`.
# The old service ran from /opt/cardos, which this does not touch.
set -euo pipefail

HOST="${HOST:-root@157.245.252.47}"
BARE=/home/cardos/cardos.git
CLONE=/home/cardos/cardos
VENV=/opt/cardos/.venv
STORE=/var/lib/cardos/store
UNIT=/etc/systemd/system/cardos-proxy.service
REMOTE_URL="$HOST:$BARE"

as_cardos() { ssh "$HOST" "cd /home/cardos && sudo -u cardos -H bash -lc $(printf %q "$1")"; }

setup() {
  echo "== 1. backup"
  ssh "$HOST" 'set -e; d=/root/backup-remotebuild-$(date +%Y%m%d-%H%M%S); mkdir -p $d; chmod 700 $d
    cp '"$UNIT"' /etc/cardos/env $d/
    tar czf $d/opt-cardos-tools.tgz -C /opt/cardos tools
    echo "   $d"'

  echo "== 2. a bare repository, group-shared so a push as root stays writable by cardos"
  ssh "$HOST" "set -e
    [ -d $BARE ] || sudo -u cardos git init -q --bare --shared=group $BARE
    find $BARE -type d -exec chmod g+s {} +
    # git refuses to act as root on a repository another user owns.
    git config --global --get-all safe.directory | grep -qx $BARE || \
      git config --global --add safe.directory $BARE"
  git remote get-url droplet >/dev/null 2>&1 || git remote add droplet "$REMOTE_URL"
  git push -q droplet master
  # Pushed as root: hand the new objects back to the service's group.
  ssh "$HOST" "chgrp -R cardos $BARE && chmod -R g+w $BARE"
  echo "   pushed master"

  echo "== 3. the working clone, on branch 'remote'"
  as_cardos "set -e
    [ -d $CLONE/.git ] || git clone -q $BARE $CLONE
    cd $CLONE
    git checkout -q -B remote origin/master
    git log --oneline -1"

  echo "== 4. PlatformIO in the service's venv"
  as_cardos "$VENV/bin/pip install -q --upgrade platformio && $VENV/bin/python -m platformio --version"

  echo "== 5. first build -- the toolchain downloads now; 30-60 minutes on one core"
  as_cardos "set -e; cd $CLONE
    # The compiler build_apps.py uses arrives with the platform, so first.
    $VENV/bin/python -m platformio pkg install -e cardputer 2>&1 | tail -2
    $VENV/bin/python tools/build_apps.py | tail -3
    $VENV/bin/python -m platformio run -e cardputer -e release 2>&1 | grep -E 'RAM:|Flash:|SUCCESS|FAILED|rror' | tail -8"

  echo "== 6. the store, filled from that build"
  ssh "$HOST" "install -d -m 750 -o cardos -g cardos /var/lib/cardos $STORE"
  build_and_publish always

  echo "== 7. the service: run from the clone, with --build --store"
  unit_and_restart
  echo
  echo "done. On the device: open Build, ask for a visible change, then /update."
  echo "Remote commits: bash tools/deploy_droplet.sh pull"
}

# The unit as this version of the repository needs it, then a restart. Safe to
# repeat, and run by `update` too: the server moved from tools/webproxy.py to
# `python -m server` on 2026-09-22, and a droplet set up before that still
# starts the old path, which no longer exists after the merge.
unit_and_restart() {
  ssh "$HOST" "set -e
    sed -i 's|^WorkingDirectory=.*|WorkingDirectory=$CLONE|' $UNIT
    sed -i 's| tools/webproxy.py | -m server |' $UNIT
    grep -q -- '--build' $UNIT || sed -i 's| --token | --build --store $STORE --token |' $UNIT
    grep -q -- '--build' $UNIT || { echo 'could not add --build to the unit'; exit 1; }
    grep -q -- ' -m server ' $UNIT || { echo 'the unit does not start python -m server'; exit 1; }
    systemctl daemon-reload
    systemctl restart cardos-proxy
    sleep 3
    systemctl is-active cardos-proxy
    grep -E '^(WorkingDirectory|ExecStart)' $UNIT | sed 's/--token [^ \"]*/--token <hidden>/'"
}

# Rebuild the clone and publish whatever changed into the store. `update`
# needs this as much as `setup` does: new master can move CAPP_API_VERSION,
# and a store still holding the old apps would hand the device binaries its
# new loader refuses.
#
# The firmware only when something outside apps/ changed since the store's
# firmware was built (server/build.py: firmware_due). It embeds every app, so
# rebuilding it for an app change offered every device an OS update that was
# only the apps again. `setup` passes always.
build_and_publish() {
  as_cardos "set -e; cd $CLONE
    $VENV/bin/python -m server.build deploy --store $STORE --firmware ${1:-auto}"
}

update() {
  git push -q droplet master
  ssh "$HOST" "chgrp -R cardos $BARE && chmod -R g+w $BARE"
  # Build's own commits are on `remote`, so this is a real merge, and the
  # service user has no git identity -- server/build.py passes one per
  # command for the same reason.
  as_cardos "set -e; cd $CLONE; git fetch -q origin
    git -c user.name='CardOS droplet' -c user.email=cardos@droplet.invalid \
      merge -q --no-edit origin/master
    git log --oneline -1"
  build_and_publish
  unit_and_restart
}

pull() {
  as_cardos "cd $CLONE && git push -q origin remote"
  git fetch -q droplet
  echo "Build's commits not yet on master:"
  git log --oneline master..droplet/remote
  echo
  echo "To take them:  git merge droplet/remote"
}

# The three copies -- this checkout, the droplet, GitHub -- made one again:
# Build's commits come here, everything goes to the droplet (rebuilt and
# republished) and to GitHub's main. Stops at the first thing that needs a
# person: uncommitted work here, or a merge that does not go through clean.
sync() {
  if [ -n "$(git status --porcelain)" ]; then
    echo "uncommitted changes here; commit or stash them first"; exit 1
  fi
  [ "$(git rev-parse --abbrev-ref HEAD)" = master ] || { echo "not on master"; exit 1; }
  pull
  git merge --no-edit droplet/remote
  git fetch -q origin
  git merge --no-edit origin/main          # anything pushed to GitHub directly
  update
  git push -q origin master:main
  echo "in sync: $(git log --oneline -1)"
}

# The dashboard (server/dash.py) on its own name, over HTTPS -- Google will
# not redirect a web sign-in anywhere else. Only /dash and /google/creds are
# exposed on that name; Build, voice and updates stay on :8080 as before.
# Needs the DNS A record for $DASH_HOST pointing here first, or certbot fails.
# Safe to repeat. With a "Web application" client JSON from the Cloud console,
# also puts its id and secret into /etc/cardos/env and restarts the service.
DASH_HOST="${DASH_HOST:-cardos.arowe.net}"
dash() {
  local json="${1:-}"
  ssh "$HOST" "set -e
    cat > /etc/nginx/sites-available/cardos-dash <<'EOF'
# CardOS dashboard: server/dash.py behind HTTPS. See tools/deploy_droplet.sh.
server {
    listen 80;
    listen [::]:80;
    server_name $DASH_HOST;
    location = /dash { proxy_pass http://127.0.0.1:8081; include /etc/nginx/cardos-dash-proxy.conf; }
    location /dash/  { proxy_pass http://127.0.0.1:8081; include /etc/nginx/cardos-dash-proxy.conf; }
    location = /google/creds { proxy_pass http://127.0.0.1:8081; include /etc/nginx/cardos-dash-proxy.conf; }
    location = / { return 302 /dash; }
    location / { return 404; }
}
EOF
    cat > /etc/nginx/cardos-dash-proxy.conf <<'EOF'
proxy_set_header Host \$host;
proxy_set_header X-Real-IP \$remote_addr;
proxy_set_header X-Forwarded-Proto \$scheme;
client_max_body_size 16k;
EOF
    ln -sf /etc/nginx/sites-available/cardos-dash /etc/nginx/sites-enabled/cardos-dash
    nginx -t -q && systemctl reload nginx
    # --nginx edits the site above to add 443 and the redirect from 80.
    certbot --nginx -n --redirect -d $DASH_HOST 2>&1 | tail -2
    touch /etc/cardos/env
    grep -q '^DASH_URL=' /etc/cardos/env || echo 'DASH_URL=https://$DASH_HOST' >> /etc/cardos/env
    grep -q '^CARDOS_STATE=' /etc/cardos/env || echo 'CARDOS_STATE=/var/lib/cardos' >> /etc/cardos/env"

  if [ -n "$json" ]; then
    # Read here, sent over ssh on stdin: the secret never lands on a command line.
    python -c '
import json, sys
d = json.load(open(sys.argv[1]))
w = d.get("web")
if not w:
    sys.exit("that is not a Web application client (no \"web\" key) -- the dashboard needs one")
print(w["client_id"]); print(w["client_secret"])' "$json" |
    ssh "$HOST" 'set -e; read -r id; read -r secret
      sed -i "/^GOOGLE_CLIENT_ID=/d; /^GOOGLE_CLIENT_SECRET=/d" /etc/cardos/env
      printf "GOOGLE_CLIENT_ID=%s\nGOOGLE_CLIENT_SECRET=%s\n" "$id" "$secret" >> /etc/cardos/env
      chmod 600 /etc/cardos/env
      systemctl restart cardos-proxy; sleep 2; systemctl is-active cardos-proxy'
  fi
  echo "dashboard: https://$DASH_HOST/dash  (password: DASH_PASSWORD in /etc/cardos/env)"
  echo "redirect URI for the Google client: https://$DASH_HOST/dash/google/callback"
}

case "${1:-}" in
  setup) setup ;;
  update) update ;;
  pull) pull ;;
  sync) sync ;;
  dash) dash "${2:-}" ;;
  *) sed -n '2,16p' "$0"; exit 2 ;;
esac
