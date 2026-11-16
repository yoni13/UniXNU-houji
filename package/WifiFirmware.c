/* houji: UniXNU-sheng 6880068 package/WifiFirmware.c without the addon gate, plus board.bin and regdb.bin. Not built yet. */
/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include "WifiFirmware.h"

/* houji: no Data addon; the Data seed (make-data-seed-linux.py) is mounted at /private/var before launchd and holds the firmware. */

/* Called on the wireless worker after the Data addon becomes available.
 * Successful ownership is transferred to its boot-lifetime firmware cache. */
int sheng_wifi_firmware_read(const char *name, uint8_t **data, size_t *size) {
  *data = NULL; *size = 0;
  /* join.txt: the unattended-test network file, present only in a build made with UNIXNU_WIFI_JOIN and read only with wifi-aj=1. */
  /* houji: this phone's board data is the single board.bin (ath12k's fallback after board-2.bin) and its regulatory database regdb.bin. */
  if (strcmp(name, "amss.bin") && strcmp(name, "amss2.bin") && strcmp(name, "m3.bin") && strcmp(name, "board-2.bin") &&
      strcmp(name, "board.bin") && strcmp(name, "regdb.bin") && strcmp(name, "join.txt")) return ENOENT;
  char path[160]; snprintf(path, sizeof(path), "%s/%s", SHENG_WIFI_FIRMWARE_ROOT, name);
  vfs_context_t ctx = vfs_context_kernel(); vnode_t vp = NULL;
  int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
  uint8_t *buffer = NULL; size_t length = 0;
  if (!error) {
    struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(vp, &va, ctx);
    if (!error && (vnode_vtype(vp) != VREG || !va.va_data_size || va.va_data_size > 32u * 1024 * 1024)) error = EINVAL;
    if (!error) {
      length = (size_t)va.va_data_size; buffer = IOMallocData(length);
      if (!buffer) error = ENOMEM;
      else {
        int resid = 0;
        error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)length, 0, UIO_SYSSPACE,
            IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error && resid) error = EIO;
      }
    }
  }
  if (vp) vnode_close(vp, FREAD, ctx);
  if (error) { if (buffer) IOFreeData(buffer, length); return error; }
  *data = buffer; *size = length; return 0;
}
