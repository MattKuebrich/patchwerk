# Publishing PATCHWERK on GitHub

The repo is `MattKuebrich/patchwerk`. The emulator is hosted by GitHub Pages from the repo's root: `index.html` is the page,
`patchwerk.fwsc` is the firmware beside it (the page's Install button fetches it), `cheatsheet.html` is its cheatsheet.

## The easy way: publish.command

One time: `brew install gh`, then `gh auth login` (choose GitHub.com, HTTPS, log in with the browser).
Then, for the first release and every later one: copy the new files over the folder, double-click **publish.command** and press Return
for the version it suggests (the one the firmware says). It builds the firmware and the page, commits, pushes, turns on GitHub Pages
the first time, tags the version and makes the release with `patchwerk.fwsc` attached and the CHANGELOG entry as the notes.
`./publish.command --dry` shows what it would do without touching GitHub. The steps below are the same thing by hand.

## First time by hand (version 1.0.0)

1. **Build the firmware and the page on your Mac** (the page needs the firmware you built):
   `./build.sh` (makes `app/build/patchwerk.fwsc`), then double-click `build_web.command`. That writes `index.html`,
   `emulator.html` and `patchwerk.fwsc` next to the README.
2. **Make an empty repo** on github.com: New repository, name `patchwerk`, Public, no README, no .gitignore, no license.
3. **Clean the submodule leftovers** (`base/` is kept as plain files; those two files point at a folder that is not in the zip):
   `rm -f base/.git base/bsp/.git .gitmodules`
4. **Commit and push** from this folder:
   ```
   git init -b main
   git add .
   git commit -m "PATCHWERK 1.0.0"
   git remote add origin https://github.com/MattKuebrich/patchwerk.git
   git push -u origin main
   ```
5. **Turn on Pages:** the repo's Settings, Pages, Source: "Deploy from a branch", Branch: `main`, folder `/ (root)`, Save.
   A minute later the emulator is at https://mattkuebrich.github.io/patchwerk/ (Web MIDI and the installer work there: it is https).
6. **Make the release:** the repo's Releases, "Create a new release", Choose a tag: type `v1.0.0` and "Create new tag",
   title `PATCHWERK 1.0.0`, paste the `v1.0.0` part of CHANGELOG.txt as the notes, drag `patchwerk.fwsc` into the files, Publish.
   (GitHub adds the source zip by itself.) With the GitHub CLI: `gh release create v1.0.0 patchwerk.fwsc --title "PATCHWERK 1.0.0" --notes-file notes.txt`

## A new version (say 1.0.1)

1. Get the new tree (the zip from `tools/release.sh 1.0.1`: its CHANGELOG.txt starts with a line `v1.0.1`) and copy its files over this folder.
2. `./build.sh`, then `build_web.command`, as in step 1 above.
3. Push it (Pages updates itself in a minute):
   ```
   git add -A
   git commit -m "PATCHWERK 1.0.1"
   git push
   ```
4. Tag and release it: Releases, new release, tag `v1.0.1`, the new CHANGELOG entry as the notes, attach the new `patchwerk.fwsc`, Publish.
   Or: `git tag v1.0.1 && git push --tags && gh release create v1.0.1 patchwerk.fwsc --title "PATCHWERK 1.0.1" --notes-file notes.txt`

Notes: `app/build/`, `web/build/` and `tests/out/` are not committed (.gitignore). `emulator.html` is the same page as `index.html`, so only `index.html` is committed. `index.html` and `patchwerk.fwsc` are, because Pages and the
Install button need them. A release's `patchwerk.fwsc` must be the same file the page was built with (build_web.command makes both together).
