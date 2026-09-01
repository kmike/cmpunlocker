# Debugging

Before you go asking in the Discord for help, here is a FAQ you should take a look at:

---

## "nvidia-smi: command not found"

- The installer likely didn't run or even failed. Re-run `sudo ./install.sh` and cold reboot.

---

## nvidia-smi shows 8192 or 10240 MiB (not 65536 or 40960)

- All the PLMs must show `0xffffffff`. Run `sudo dmesg | grep SEC2_DEBUG`to confirm.

- If this still persists, refer to the Discord protocol at the end of the document.

---

## PCIe still at Gen1 after install

First, verify with the honest indicator (never `nvidia-smi`, which caches
probe-time values):

    sudo ./tools/watch-setup.sh verify     # reads LnkSta per card

If Gen1 persists:

- Confirm IOMMU passthrough mode is enabled (the installer configures it;
  `--no-iommu` skips it).
- Find the watcher trace: `journalctl -b 0 | grep "flip detected"` (initramfs
  stage; also `/var/log/cmp-gen2-watch.log` for the userspace stage). You
  should see one flip + retrain per card. No flip line at all means the
  driver patches did not open the advertisement window (check dmesg for the
  SEC2_DEBUG lines); a flip without a resulting `LnkSta` speed change means
  the retrain did not take.
- A structural cause measured on one rig (and matching at least one
  community report): the old timer-based hammer fired after the ~0.4 s
  advertisement window had already closed, because the module loading
  from the initramfs puts the window before systemd starts. The two-stage
  advertisement-triggered watcher replaced it precisely for that case —
  see [gen2-window.md](gen2-window.md) for the measured mechanism,
  including why a late retrain fails silently (a retrain whose effective
  target equals the current speed is a spec-level no-op).
- If this still persists, refer to the Discord protocol at the end of the document.

---

## Discord protocol

If you have tried the above steps and are still having issues, please follow these steps to get help in the [Discord community](https://discord.gg/CdHSakKSFv):

1. Open a ticket in the #issue-support channel.

2. Provide the following information in your ticket:
   - Your operating system and version
   - Your GPU model and driver version
   - The output of `sudo dmesg | grep SEC2_DEBUG`
   - Latest install log (if applicable)

