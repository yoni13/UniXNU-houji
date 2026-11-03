#!/usr/bin/env python3
import hashlib, json, os, plistlib, shutil, sys
from pathlib import Path
import guestfs
image=Path(sys.argv[1]); art=Path(sys.argv[2]); out=Path(sys.argv[3])
bundle='/System/Library/Extensions/NativeAdrenoMetal.bundle'
g=guestfs.GuestFS(python_return_dict=True); g.set_network(False); g.set_memsize(2048); g.set_smp(4); g.add_drive_opts(str(image),format='raw'); g.launch(); g.mount_vfs('force,rw','hfsplus','/dev/sda','/')
def put(src,dst,mode):
    if g.exists(dst): g.rm(dst)
    g.upload(str(src),dst); g.chown(0,0,dst); g.chmod(mode,dst)
try:
    g.mkdir_p(bundle); g.chown(0,0,bundle); g.chmod(0o755,bundle)
    put(art/'NativeAdrenoMetal.bundle/NativeAdrenoMetal',bundle+'/NativeAdrenoMetal',0o755)
    put(art/'NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib',bundle+'/libvulkan_freedreno.dylib',0o755)
    put(art/'NativeAdrenoMetal.bundle/Info.plist',bundle+'/Info.plist',0o644)
    g.mkdir_p(bundle+'/_CodeSignature'); g.chown(0,0,bundle+'/_CodeSignature'); g.chmod(0o755,bundle+'/_CodeSignature')
    put(art/'NativeAdrenoMetal.bundle/_CodeSignature/CodeResources',bundle+'/_CodeSignature/CodeResources',0o644)
    put(art/'NativeAdrenoMetal.clients','/System/Library/Extensions/NativeAdrenoMetal.clients',0o644)
    put(art/'NativeAdrenoMetal.publish','/System/Library/Extensions/NativeAdrenoMetal.publish',0o644)
    # A private writable fallback for backboardd's shader/initialization logs; plugin still tries standard dirs first.
    cache='/private/var/mobile/Library/Caches'
    if not g.exists(cache): g.mkdir_p(cache)
    g.chown(501,501,cache); g.chmod(0o700,cache)
    g.sync();
    rows={}
    for p in [bundle+'/NativeAdrenoMetal',bundle+'/libvulkan_freedreno.dylib',bundle+'/Info.plist',bundle+'/_CodeSignature/CodeResources','/System/Library/Extensions/NativeAdrenoMetal.clients','/System/Library/Extensions/NativeAdrenoMetal.publish']:
        st=g.statns(p); data=g.read_file(p); rows[p]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'uid':st['st_uid'],'gid':st['st_gid'],'mode':oct(st['st_mode']&0o777)}
    assert plistlib.loads(g.read_file(bundle+'/Info.plist'))['NSPrincipalClass']=='NativeAdrenoMetalDevice'
    assert g.read_file('/System/Library/Extensions/NativeAdrenoMetal.clients')==b'backboardd\n'
    assert g.statns(cache)['st_uid']==501 and (g.statns(cache)['st_mode']&0o777)==0o700
    g.umount_all(); g.shutdown()
finally: g.close()
out.write_text(json.dumps({'image':str(image),'files':rows,'backboardd_enabled':True,'publish_enabled':True},indent=2)+'\n')
