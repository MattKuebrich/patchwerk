#!/bin/sh
# Puts the firmware's link and checksum back into index.html (the page built without it).
# Run it in the patchwerk folder (the one with index.html and patchwerk-v….fwsc), then ./publish.command --site
cd "$(dirname "$0")" || exit 1
fw=$(ls patchwerk-v*.fwsc 2>/dev/null | tail -1)
[ -n "$fw" ] || { echo "No patchwerk-v….fwsc in this folder."; exit 1; }
grep -q "FWSC_META = '/\*__FWSC__\*/'" index.html || { echo "index.html already has its firmware link: nothing to do."; exit 0; }
python3 - "$fw" <<'PY'
import hashlib, json, re, sys
fw = sys.argv[1]
ver = re.search(r'patchwerk-(v[0-9.]+)\.fwsc', fw).group(1)
meta = json.dumps({'pkg': fw, 'sha256': hashlib.sha256(open(fw, 'rb').read()).hexdigest(), 'version': ver})
for page in ('index.html',):
    s = open(page, encoding='utf-8').read()
    s = s.replace("'/*__FWSC__*/'", "'" + meta + "'", 1)
    open(page, 'w', encoding='utf-8').write(s)
print('index.html now links', fw)
PY
grep -q "FWSC_META = '/\*__FWSC__\*/'" index.html && { echo "Did not work."; exit 1; }
echo "Done. Now run: ./publish.command --site"
