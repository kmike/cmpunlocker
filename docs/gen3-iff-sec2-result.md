# Gen3 on the 8 GB CMP 170HX: the SEC2-sourced IFF probe, and its result

## Why this probe exists

On a 10 GB card the working Gen3 chain is:

1. RIR clears `DISABLE_SW_OVERRIDE` (hard column fuse, row 0 bit 5) — permanent, 2 records
2. `EN_SW_OVERRIDE = 1` then sticks
3. `IFF_SW_FUSING` becomes a general fuse-value editor — volatile, per boot

Step 1 is impossible on an 8 GB card: the RIR repair macro is factory-spent, 0 of 16 records
free on every 8 GB die measured. So the question was whether step 1 can be *substituted* rather
than performed — specifically whether the veto is **source-gated** rather than
privilege-gated.

That was not a blind guess. On this silicon `NV_XVE_VSEC_DEVICE` bit 0 is provably
Booter-writable yet is refused at L3-from-XVE, which established that source gating exists
here. And every fuse PLM on this card already reads `0xFFFFFFFF`, so privilege was never what
blocked the IFF commit.

## The control (host-sourced), measured on 0000:80:00.0

```
DISABLE_SW_OVR_STATUS 0x820084 = 1          veto in place
EN_SW_OVERRIDE := 1  -> reads 1             arms fine
IFF_SW_FUSING  := 1  -> stays 1 for 1000 ms, never self-clears
row 507  0xA0802007 -> 0xA0802007           unchanged
```

## The probe (SEC2-sourced)

`driver/patches/pcie-gen3-iff-sec2.patch` drives the identical four writes through the Booter
payload path — `kgspSec2PostblTimingRefillPayload` + `kgspExecuteBooterLoad_HAL`, one
SEC2-HS-sourced BAR0 write per Booter run.

Safety by construction: no `FUSECTRL` `CMD_WRITE`, GPIO19 never raised, IFF rows are volatile
SRAM rather than OTP, and the payload written is the target row own current value (read back
through the fuse controller first), so even a successful commit is a no-op.

## Result

```
GEN3_IFF: pre  DISABLE_SW_OVR=1 EN_SW_OVR=0x00000000 IFF_SW_FUSING=0x00000000
               IFF_RECORD=0x01fb01e8 GEN3_DIS=1 row=507 rowval=0xa0802007
GEN3_IFF: [0] EN_SW_OVERRIDE(0x820040) := 0x00000001 readback=0x00000001
GEN3_IFF: [1] FUSEADDR(0x820004)       := 0x000001fb readback=0x000001fb
GEN3_IFF: [2] FUSEWDATA_SELF(0x82000c) := 0xa0802007 readback=0x00000000   (write-only reg)
GEN3_IFF: [3] IFF_SW_FUSING(0x820088)  := 0x00000001 readback=0x00000001
GEN3_IFF: polled IFF_SW_FUSING 200000 times before reading 0x00000001      (~692 ms)
GEN3_IFF: RESULT IFF_SW_FUSING=0x00000001 self_cleared=NO-veto-holds GEN3_DIS 1 -> 1
```

**The transport worked.** `EN_SW_OVERRIDE` and `FUSEADDR` both read back their written values,
so SEC2-HS demonstrably reached the fuse block.

**The veto held anyway.** Maximum privilege, correct source, all PLMs open, observed for
~692 ms against the control 1000 ms — and `IFF_SW_FUSING` latched at 1 exactly as it does from
the host.

## Conclusion

The SW-override veto is enforced **inside the fuse block, for every requester**. It is not
source-gated the way `NV_XVE_VSEC_DEVICE` is. Substituting SEC2 privilege for the RIR write
does not work.

This also settles a dispute in the community channel: arming `EN_SW_OVERRIDE` is real and
happens without touching `DISABLE_SW_OVERRIDE`, but it is not sufficient — the IFF **commit**
is what the veto blocks, and it blocks it from SEC2 too.

For an 8 GB card, Gen3 through this route requires a free RIR record. There is not one.
