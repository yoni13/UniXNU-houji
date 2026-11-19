# houji: Turnip patches kept in the device repository, applied once to mesa-work and recorded in the pinned patch list.
for T in $HOME/b/UniXNU/device/scripts/turnip-darwin-free-during-submit.patch $HOME/b/UniXNU/device/scripts/turnip-darwin-skip-freed-entries.patch; do
  [ -f $T ] || continue; B=patches/mesa/$(basename $T); cp $T $B
  git -C .cache/mesa-work apply -R --check $PWD/$B 2>/dev/null || git -C .cache/mesa-work apply $PWD/$B || exit 1
  /opt/local/bin/python3.13 -c "import json,sys;p='scripts/data/adreno-compiler-inputs.json';d=json.load(open(p));b=sys.argv[1]
if b not in d['patches']: d['patches'].append(b); open(p,'w').write(json.dumps(d,indent=2)+'\n')" $B
done
