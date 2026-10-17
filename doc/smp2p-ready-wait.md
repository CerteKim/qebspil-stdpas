# Wait for the remoteprocs' SMP2P "ready" instead of a fixed settle

Status: **spec / TODO** (2026-10-17). The current build waits a fixed
`QEBSPIL_SETTLE_MS` (5000 ms) after `pil_finish_all()`, before returning from
`ExitBootServices` (after which slbounce does the Secure Launch and takes the
hypervisor away). Measured: 2000 ms is too short (the DSP is killed mid-handover
-> the kernel sees `SMP2P ... -19` and never attaches), 5000 ms works. Polling
the actual "ready" signal would be deterministic and save 2-3 s per boot.

## What to poll

Read the SMP2P *inbound* item of each started remoteproc from SMEM and test the
`ready` entry's bit 0. This is the same signal the kernel uses in
`qcom_q6v5_read_smp2p_state()` (via `irq_get_irqchip_state()` on the smp2p
irqchip), so agreement is guaranteed.

## Data structures (verified against linux 7.2)

SMEM region: the `/reserved-memory` child whose name starts with `smem`
(sc8180x: `smem@86000000`, 2 MiB). Everything is little-endian.

```c
struct smem_header {                 /* new (TOC) layout */
	__le32 proc_comm[8];         /* 4 x {command,status} = 32 bytes */
	__le32 version[32];          /* 128 bytes */
	__le32 initialized;
	__le32 free_offset;
	__le32 available;
	__le32 reserved;
	struct smem_global_entry toc[];   /* offset 0xb0 = 176 */
};
struct smem_global_entry {
	__le32 allocated;
	__le32 offset;               /* from the start of the SMEM region */
	__le32 size;
	__le32 aux_base;
};
```

The TOC is indexed by the **item ID** (`qcom,smem` in the DT), not by host.

```c
struct smp2p_smem_item {
	u32 magic;                   /* 0x504d5320 "SMP " */
	u8 version;
	unsigned features:24;
	u16 local_pid;
	u16 remote_pid;
	u16 total_entries;
	u16 valid_entries;
	u32 flags;
	struct {                     /* at offset 20, 20 bytes each, 16 of them */
		char name[16];
		u32 value;
	} entries[16];
} __packed;
```

## Algorithm

1. Find the SMEM base/size from the DT (`/reserved-memory/smem@...` `reg`).
2. For every remoteproc that `pil_finish_all()` actually started:
   a. Walk the DT for `compatible = "qcom,smp2p"` nodes with `qcom,remote-pid`
      equal to that remote's pid (keep a pid in the `pil_type` table:
      ADSP 2, SLPI 3, CDSP 5 — same values the vendor DTB uses).
   b. Read `qcom,smem = <out_id>, <in_id>`; take `in_id`.
   c. `toc[in_id]`: skip if `!allocated` / out of range.
   d. Check `magic == 0x504d5320` and `local_pid == pid || remote_pid == pid`.
   e. Find the entry named `ready` (within `valid_entries`), test `value & 1`.
3. Poll every 50 ms until **all** started remotes are ready or a 10 s deadline
   expires.
4. Log one line per remote (`ready after N ms` / `timeout`) and then:
   * all ready -> return immediately;
   * otherwise -> fall back to the fixed `QEBSPIL_SETTLE_MS` stall (so a wrong
     guess about SMEM can never make a boot fail).

## Safety

Only **RAM reads** — no MMIO, no writes, no faults. Any parsing problem must
degrade to the fixed settle, never to a hang. Build it behind
`QEBSPIL_WAIT_SMP2P=0|1` so the known-good fixed-settle build stays the default
until the poll has been tested on the machine.

## Acceptance

`qebspil.log` shows `SMP2P ready: adsp after <N> ms` with N well below 5000,
then the kernel attaches (`SMP2P ready irq ... state 1` -> `adsp is now
attached`) on repeated boots, including immediately after a cold boot.
