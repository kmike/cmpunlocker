#!/bin/sh
# pre-udev: start the watcher detached before coldpick/module load.
# Output to /dev/kmsg (unbuffered, one line per write) so the trace
# lands in the kernel journal and survives switch_root. The process
# dies at switch_root by design; the userspace unit takes over after
# pivot for rootfs-loaded-module shapes.
exec /usr/bin/cmp-gen2-watch --duration-ms 60000 >/dev/kmsg 2>&1 &
