#!/bin/bash
# sly1-update.sh: brings upstream ProjectCane (theclub654/ProjectCane) into the Sly 1 fork's `safehouse` branch and
# points this checkout's Sly1/ submodule at the result. Nothing is pushed.
#
#   tools/sly1-update.sh [--fork DIR] [--date YYYY-MM-DD] [--no-rebase] [--pin]
#
#   --fork DIR     the scratch clone of the fork to work in (default: build/sly1-fork in this checkout; cloned from
#                  .gitmodules' URL when missing). Never the user's own fork checkout.
#   --date DATE    the tag's date (default: today): safehouse-DATE
#   --no-rebase    only fetch and report what upstream has that `master` does not
#   --pin          afterwards, check out the new `safehouse` in Sly1/ and stage the gitlink here (not committed)
#
# The fork's branches (notes/SLY1_PLAN_2026-11-20.md §3):
#   master     a pure mirror of upstream's master: only fast-forwarded, never committed to
#   safehouse  SafeHouse's patch series on top of master, kept as small reviewable commits; updated by REBASE
#   safehouse-YYYY-MM-DD  a tag on the series' head before each rebase, so the commits older SafeHouse revisions pin
#              stay reachable after the force-push
#
# Steps: fetch upstream -> master fast-forwarded to upstream/master -> tag the current safehouse -> rebase safehouse onto
# master (git rerere on: a conflict resolved once is replayed next time) -> summary. A conflict stops the script inside
# the rebase: resolve it in the fork clone (git status there), `git rebase --continue`, then run with --pin.
#
# Afterwards (by hand): build and run the Sly 1 checks (docs/BUILDING.md), commit the gitlink here naming the upstream
# range, and push the fork (the user does: git push --force-with-lease origin safehouse master safehouse-DATE).
# Warns when upstream changed the extractor's formats (File Extractor GUI/Services, PCBRX-*.md): SafeHouse's own
# cane-extractor (File Extractor/pcbrx.h) and platform/install.h's asset version must follow those by hand.
set -euo pipefail

SRC=$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)
FORK=$SRC/build/sly1-fork
DATE=$(date +%Y-%m-%d)
REBASE=1; PIN=0
UPSTREAM_URL=https://github.com/theclub654/ProjectCane.git
while [ $# -gt 0 ]; do
  case "$1" in
    --fork) FORK=$2; shift ;;
    --date) DATE=$2; shift ;;
    --no-rebase) REBASE=0 ;;
    --pin) PIN=1 ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "sly1-update: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
FORK_URL=$(git -C "$SRC" config -f "$SRC/.gitmodules" --get submodule.Sly1.url)

if [ ! -d "$FORK/.git" ]; then
  echo "sly1-update: cloning $FORK_URL into $FORK"
  git clone -q "$FORK_URL" "$FORK"
  git -C "$FORK" checkout -q -B safehouse origin/safehouse
fi
F() { git -C "$FORK" "$@"; }
F config rerere.enabled true
F config rerere.autoupdate true
F remote get-url upstream > /dev/null 2>&1 || F remote add upstream "$UPSTREAM_URL"
F fetch -q upstream
F fetch -q origin 2>/dev/null || echo "sly1-update: (could not fetch origin; using the clone's own branches)"
[ -z "$(F status --porcelain)" ] || { echo "sly1-update: $FORK has uncommitted changes" >&2; exit 1; }

OLD_MASTER=$(F rev-parse master)
NEW=$(F rev-list --count master..upstream/master)
echo "sly1-update: upstream has $NEW new commit(s) on top of master ($(F rev-parse --short master))"
F log --oneline master..upstream/master | sed 's/^/  /'
for path in "File Extractor GUI/Services" "File Extractor GUI/NativeTools" PCBRX-v19.md; do
  if [ -n "$(F diff --name-only master upstream/master -- "$path")" ]; then
    echo "sly1-update: WARNING: upstream changed $path: check File Extractor/pcbrx.h and platform/install.h (assetVersion)"
  fi
done
if [ $REBASE = 1 ]; then

# master: fast-forward only (it is a mirror)
F merge-base --is-ancestor master upstream/master ||
  { echo "sly1-update: master has commits upstream does not: it must stay a mirror" >&2; exit 1; }
F branch -f master upstream/master 2>/dev/null || { F checkout -q master && F merge -q --ff-only upstream/master; }

# tag, then rebase the series
TAG=safehouse-$DATE
if F rev-parse -q --verify "refs/tags/$TAG" > /dev/null; then
  [ "$(F rev-parse "$TAG^{commit}")" = "$(F rev-parse safehouse)" ] ||
    { echo "sly1-update: tag $TAG exists on another commit (--date to choose another)" >&2; exit 1; }
else
  F tag "$TAG" safehouse
fi
F checkout -q safehouse
echo "sly1-update: rebasing safehouse ($(F rev-list --count "$OLD_MASTER"..safehouse) commits) onto master"
if ! F rebase -q --onto master "$OLD_MASTER" safehouse; then
  echo "sly1-update: CONFLICT. Resolve it in $FORK (git status), git rebase --continue, then: $0 --fork $FORK --no-rebase --pin" >&2
  exit 1
fi
F log --oneline master..safehouse | sed 's/^/  /'
fi

if [ $PIN = 1 ]; then
  [ -e "$SRC/Sly1/.git" ] || git -C "$SRC" submodule update --init Sly1
  git -C "$SRC/Sly1" fetch -q "$FORK" safehouse
  git -C "$SRC/Sly1" checkout -q FETCH_HEAD
  git -C "$SRC" add Sly1
  echo "sly1-update: Sly1/ now at $(F rev-parse --short safehouse) (staged; commit it after the build and checks)"
fi
echo "sly1-update: done. Push (user only): git -C $FORK push --force-with-lease origin safehouse master safehouse-$DATE"
