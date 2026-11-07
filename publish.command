#!/bin/bash
# Double-click on a Mac: builds the firmware and the page, commits, pushes to GitHub (MattKuebrich/patchwerk), turns on
# GitHub Pages the first time, and makes the release (tag, notes from CHANGELOG.txt, patchwerk-vX.Y.Z.fwsc attached).
# First time: brew install gh, then gh auth login. Run it from the folder that holds this file.
# To try it without changing anything on GitHub: ./publish.command --dry
OWNER=MattKuebrich; REPO=patchwerk
cd "$(dirname "$0")" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
DRY=; [ "$1" = "--dry" ] && { DRY=1; shift; }
pause() { echo; read -n 1 -s -r -p "Press any key to close."; echo; }
fail() { echo; echo "STOPPED: $1"; pause; exit 1; }
run() { if [ -n "$DRY" ]; then echo "  (dry) $*"; else "$@"; fi; }

command -v git >/dev/null || fail "git is not installed (xcode-select --install)."
command -v gh >/dev/null || fail "the GitHub CLI is missing: brew install gh"
[ -n "$DRY" ] || gh auth status >/dev/null 2>&1 || fail "not signed in to GitHub: run  gh auth login  once, then try again."

# the version: the one the firmware says, unless you type another
cur=$(grep -o '"v[0-9][0-9.]*"' app/core/pw_ui.h | head -1 | tr -d '"v')
[ -n "$cur" ] || fail "the firmware's version (PW_VERSION in app/core/pw_ui.h) is not a plain number like v1.0.0."
ver="${1:-}"
if [ -z "$ver" ]; then read -r -p "Version to publish [$cur]: " ver; ver="${ver:-$cur}"; fi
ver="${ver#v}"
echo "$ver" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || fail "the version should look like 1.0.1"
[ "$ver" = "$cur" ] || fail "the firmware says v$cur but you chose v$ver: use the version the new files were made with."
head -1 CHANGELOG.txt | grep -qx "v$ver" || fail "CHANGELOG.txt does not start with a line  v$ver  (the new files bring it)."
if [ -d .git ] && git rev-parse "v$ver" >/dev/null 2>&1; then fail "v$ver is already tagged: it has been published. Use a new version."; fi

echo "== Building the firmware"
bash ./build.sh || fail "the firmware build failed (see above)."
[ -f app/build/patchwerk.fwsc ] || fail "app/build/patchwerk.fwsc was not made."
echo "== Building the page (index.html) with that firmware beside it"
NOPAUSE=1 bash ./build_web.command || fail "the page build failed (see above)."
fw="patchwerk-v$ver.fwsc"
[ -f index.html ] && [ -f "$fw" ] || fail "index.html or $fw was not made."
cmp -s app/build/patchwerk.fwsc "$fw" || fail "$fw beside the page is not the firmware just built."
[ -n "$DRY" ] || ls patchwerk*.fwsc | grep -vx "$fw" | while read -r old; do rm -f "$old"; done   # (one firmware in the repo: the page's)

echo "== Git"
newrepo=
if [ ! -d .git ]; then
    newrepo=1
    run rm -f base/.git base/bsp/.git .gitmodules   # (the submodule leftovers: base/ is kept as plain files)
    run git init -b main || fail "git init failed."
fi
notes=$(mktemp)
awk -v v="v$ver" 'NR==1{next} /^v[0-9]/{exit} {print}' CHANGELOG.txt | sed 's/^  - /- /' > "$notes"
if [ -n "$newrepo" ] || ! git remote get-url origin >/dev/null 2>&1; then
    if [ -n "$DRY" ]; then echo "  (dry) create github.com/$OWNER/$REPO if it does not exist; add it as origin"
    else
        gh repo view "$OWNER/$REPO" >/dev/null 2>&1 || gh repo create "$OWNER/$REPO" --public --description "A patchable modular synth for the M-VAVE FM-1" >/dev/null || fail "could not create the repo $OWNER/$REPO."
        git remote add origin "https://github.com/$OWNER/$REPO.git" 2>/dev/null
    fi
fi
run git add -A
if [ -n "$DRY" ] || ! git diff --cached --quiet; then run git commit -q -m "PATCHWERK $ver" || fail "git commit failed."; else echo "  (nothing new to commit)"; fi
run git push -u origin main || fail "git push failed (is the repo empty or yours?)."
if [ -n "$newrepo" ]; then   # GitHub Pages, from the main branch's root (the first time only)
    echo "== Turning on GitHub Pages"
    run gh api -X POST "repos/$OWNER/$REPO/pages" -f "source[branch]=main" -f "source[path]=/" >/dev/null 2>&1 || echo "  (Pages may already be on, or turn it on in Settings, Pages: branch main, folder / (root))"
fi
echo "== Release v$ver"
run git tag "v$ver" || fail "could not tag v$ver."
run git push origin "v$ver" || fail "could not push the tag."
run gh release create "v$ver" "$fw" --repo "$OWNER/$REPO" --title "PATCHWERK $ver" --notes-file "$notes" || fail "could not make the release (the tag is pushed: make it on github.com, Releases)."
rm -f "$notes"
echo
echo "Done."
echo "  Release: https://github.com/$OWNER/$REPO/releases/tag/v$ver"
echo "  Page:    https://$(echo $OWNER | tr 'A-Z' 'a-z').github.io/$REPO/  (a minute after the first push)"
pause
