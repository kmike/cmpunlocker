# The Gen2 advertisement window

Measured mechanism behind the advertisement-triggered retrain
(`tools/cmp-gen2-watch.c`). All timings from a GA100 CMP 170HX
(`10de:20c2`), driver `610.43.02`, dual-socket server board
(SNB-EP/C602), Sep 2026. The model should hold generally; the numbers
are from one rig.

## Mechanism

The Gen2 driver patch (`driver/patches/pcie-gen2.patch`) flips the
endpoint's `LnkCap`/`LnkCtl2` to Gen2 during GSP bootstrap. On some
driver/platform combinations firmware reverts the flip about 0.4 s
later. Outside that window the endpoint advertises Gen1, and a retrain
against it lands Gen1 no matter what the root port wants. On other
combinations the flip persists at runtime — those rigs train Gen2
with any late retrain, which is why the probe-time retrain
(`driver/patches/pcie-gen2-probe-retrain.patch`) alone appears to
work for some users and not others.

Where the window sits depends on when the module loads. If the NVIDIA
module loads from the initramfs, GSP bootstrap — and the window —
happen before systemd userspace starts (measured: window at T+5.4 s,
systemd at T+13 s on a dual-socket server with LVM), and no userspace
service or `setpci` one-shot can reach it. If the module loads from
the root filesystem, the window opens after userspace is up and late
retrains can work.

A retrain whose effective target equals the current speed is a silent
no-op: writing Retrain Link while both endpoints' `LnkCtl2` resolve to
the already-active speed does nothing observable — no Recovery, no
link event, no error. A late retrain therefore fails silently, which
is easy to misdiagnose as "the platform refuses Gen2". Measured: nine
in-window retrains with the parent port target still at Gen1 produced
zero link events.

## The fix

Poll each card's `LnkCap` at 1 kHz. When a card advertises >= Gen2,
raise its parent port's `LnkCtl2` target and fire Retrain Link every
50 ms for as long as the flip is live.

Two deployment stages, so the watcher does not depend on where the
window lands:

- initramfs hooks (dracut module + initramfs-tools): the watcher is
  spawned before udev coldplug and survives the pivot to the real
  root (observed under both dracut and classic init), covering every
  window from initramfs start to its 60 s bound;
- a systemd userspace unit as a second net, pulled in at
  sysinit.target with DefaultDependencies=no (measured T+9-15 s at
  multi-user.target on the test rig vs T+13 s with the early
  ordering — early boot there is CPU-bound).

Between the two stages, window coverage is continuous from initramfs
start to T+90 s. The trigger is the card's advertisement state, not a
timer: if the flip persists at runtime instead of reverting, the
userspace stage catches it whenever it appears.

## Persistence

Once a link trains Gen2 inside the window, its `LnkCap` advertisement
stays flipped at runtime; the revert only afflicts boots whose window
was missed. On the rigs measured here (dual-card, warm and cold
boots) both links trained in-window and both `LnkCap` read 5 GT/s at
runtime hours later. Treat runtime persistence as evidence of an
in-window train, not as proof.

## Verification

- Trust `LnkSta` (`lspci -vv`, `setpci CAP_EXP+12.w`), not `LnkCap`,
  and not `nvidia-smi` (it caches probe-time values).
- Ground truth: host bandwidth roughly doubles at Gen2 x4
  (~0.8 → ~1.5-1.7 GB/s per direction).
- `tools/watch-setup.sh verify` prints the per-card state.

## Debugging notes

- Finding the initramfs-stage trace: `journalctl -b 0 | grep "flip
  detected"` — lines appear both as unit stdout (dracut-pre-udev[..])
  and as unattributed kmsg records (prefix "unknown:"). Search for
  content you expect in the records ("flip detected", "via parent"),
  not the binary name. The userspace stage logs to
  `/var/log/cmp-gen2-watch.log`.
- The watcher is stdio-unbuffered on purpose: buffered logs are lost
  when the initramfs is torn down at `switch_root`.
- Do not pipe the initramfs-stage watcher through `tee` or other
  coreutils — minimal initramfs images may not contain them.
