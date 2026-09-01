# The Gen2 advertisement window (why retrains fail on some rigs)

This documents the measured mechanism behind the advertisement-triggered
retrain (`tools/cmp-gen2-watch.c`). All timings measured on a GA100
CMP 170HX (`10de:20c2`), driver `610.43.02`, dual-socket server board
(SNB-EP/C602), Sep 2026; the qualitative model holds generally.

## The three facts

1. **The patch opens a short window, not a persistent state.** Patch
   `0007` flips the endpoint's `LnkCap`/`LnkCtl2` to Gen2 during GSP
   bootstrap. On some driver/platform combinations firmware reverts the
   flip roughly **0.4 s later**. Outside that window the endpoint
   advertises Gen1, and a retrain against it lands Gen1 no matter what
   the root port wants. (On other combinations the flip persists at
   runtime — those rigs train Gen2 with any late retrain, which is why
   `0008` alone appears to work for some users and not others.)

2. **Where the window sits depends on boot shape.** If the NVIDIA
   module loads from the initramfs, GSP bootstrap — and the window —
   happen *before* systemd userspace starts (observed: window at
   T+5.4 s, systemd at T+13 s on a dual-socket server with LVM). No
   userspace service, hammer loop, or `setpci` one-shot can reach that
   window. On fast consumer boards the module loads later and the
   window lands inside a userspace hammer's reach — which is why the
   old timer-based hammer worked on reference hardware and never on
   servers.

3. **A retrain whose effective target equals the current speed is a
   silent no-op.** Writing Retrain Link while the target link speed
   (both endpoints' `LnkCtl2`) resolves to the already-active speed
   does nothing observable: no Recovery, no link event, no error. A
   late retrain therefore fails *silently*, which is easily
   misdiagnosed as "the platform refuses Gen2". (Measured: nine
   in-window retrains with the parent port target still at Gen1
   produced zero link events.)

## The fix

Poll each card's `LnkCap` at 1 kHz. The instant a card advertises
>= Gen2, raise its parent port's `LnkCtl2` target and fire Retrain
Link every 50 ms for as long as the flip is live. A link that trains
Gen2 inside the window **stays** Gen2 after the window closes (the
trained link state survives the advertisement reverting).

Deployed at two stages so it works for every boot shape:

- initramfs hooks (dracut module + initramfs-tools) — for windows that
  open during initramfs module load;
- a systemd userspace unit — for windows that open after pivot.

State-triggered, never timer-blind: if the flip persists at runtime
instead of reverting, the userspace stage catches it whenever it
appears.

## Verification rules

- Trust `LnkSta` (`lspci -vv`, `setpci CAP_EXP+12.w`), never `LnkCap`,
  and never `nvidia-smi` cached probe values.
- Ground truth: host bandwidth roughly doubles at Gen2 x4
  (~0.8 → ~1.5–1.7 GB/s per direction).
- `tools/watch-setup.sh verify` prints the honest per-card state.

## Debugging notes

- The initramfs stage logs to the kernel journal
  (`journalctl -k -b 0 | grep cmp-gen2-watch`); the userspace stage to
  `/var/log/cmp-gen2-watch.log`.
- The watcher is stdio-unbuffered on purpose: buffered logs are lost
  when the initramfs is torn down at `switch_root`.
- Do not pipe the initramfs-stage watcher through `tee` or other
  coreutils — minimal initramfs images may not contain them.
