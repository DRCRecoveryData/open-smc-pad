#!/usr/bin/env bash
# Publish the web UI's static copy to the public Pages repo (the source repo is private,
# and Pages on a private repo needs a paid plan).
#
#   host/webui/publish_pages.sh
#
# Needs git credentials for the public repo (gh auth setup-git, or a token in the URL).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO="${SMCPAD_WEBUI_REPO:-DRCRecoveryData/smcpad-webui}"
GIT_NAME="${SMCPAD_GIT_NAME:-Federico Paglioni}"
GIT_EMAIL="${SMCPAD_GIT_EMAIL:-paglioni.federico@gmail.com}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! git clone -q --depth 1 "https://github.com/$REPO.git" "$TMP/site" 2>/dev/null; then
    mkdir -p "$TMP/site"
    git -C "$TMP/site" init -q -b main
    git -C "$TMP/site" remote add origin "https://github.com/$REPO.git"
fi

cp "$ROOT/host/webui/index.html" "$TMP/site/"
mkdir -p "$TMP/site/static"
cp "$ROOT/host/webui/static/"*.png "$TMP/site/static/"
touch "$TMP/site/.nojekyll"

cd "$TMP/site"
git add -A
if git diff --cached --quiet; then
    echo "already up to date"
    exit 0
fi
git -c user.name="$GIT_NAME" -c user.email="$GIT_EMAIL" \
    commit -q -m "webui: publish $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || date -u +%Y-%m-%dT%H:%MZ)"
git push -q origin main
echo "published: https://drcrecoverydata.github.io/smcpad-webui/"
