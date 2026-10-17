// SPDX-License-Identifier: GPL-2.0-only
/*
 * Wait for the remoteprocs to publish their SMP2P "ready" entry before the
 * Secure Launch switch takes the hypervisor away.
 *
 * See doc/smp2p-ready-wait.md for the data layout (verified against linux 7.2)
 * and why this has to happen here rather than in the kernel. Only RAM is read:
 * no MMIO, no writes, and every failure path falls back to the fixed settle.
 */
#include <efi.h>
#include <efilib.h>
#include <libfdt.h>

#include "qebspil.h"

#define SMEM_TOC_OFFSET		0xb0	/* proc_comm + version[32] + 4 words */
#define SMP2P_MAGIC		0x504d5320u	/* "SMP " */
#define SMP2P_MAX_ENTRY		16
#define SMP2P_ENTRY_OFFSET	20
#define SMP2P_ENTRY_SIZE	20
#define SMP2P_NAME_LEN		16

#define WAIT_DEADLINE_MS	10000
#define WAIT_POLL_MS		50

struct smem_global_entry {
	UINT32 allocated;
	UINT32 offset;
	UINT32 size;
	UINT32 aux_base;
};

struct smp2p_item {
	UINT32 magic;
	UINT8 version;
	UINT8 features[3];
	UINT16 local_pid;
	UINT16 remote_pid;
	UINT16 total_entries;
	UINT16 valid_entries;
	UINT32 flags;
} __attribute__((packed));

/* remote pids of the DSPs qebspil knows how to start */
static const struct {
	const char *compatible;
	UINT16 pid;
} rproc_pids[] = {
	{ "qcom,sc8180x-adsp-pas", 2 },
	{ "qcom,sc8180x-slpi-pas", 3 },
	{ "qcom,sc8180x-cdsp-pas", 5 },
};

static const void *smem_base(const void *dtb, UINT32 *out_size)
{
	int rmem, node, len;
	const char *name;

	rmem = fdt_path_offset(dtb, "/reserved-memory");
	if (rmem < 0)
		return NULL;

	fdt_for_each_subnode(node, dtb, rmem) {
		const UINT32 *reg;

		name = fdt_get_name(dtb, node, NULL);
		if (!name || strncmp(name, "smem", 4))
			continue;

		reg = fdt_getprop(dtb, node, "reg", &len);
		if (!reg || len < (int)(4 * sizeof(UINT32)))
			return NULL;

		/* 2 address cells + 2 size cells; SMEM lives below 4 GiB */
		*out_size = fdt32_to_cpu(reg[3]);
		return (const void *)(UINTN)fdt32_to_cpu(reg[1]);
	}

	return NULL;
}

/* Does this smp2p item carry a "ready" entry that is set? */
static BOOLEAN item_ready(const void *base, UINT32 smem_size, UINT32 item)
{
	const struct smem_global_entry *toc;
	const struct smp2p_item *hdr;
	UINT32 offset;
	UINT16 valid;
	UINTN i;

	toc = (const struct smem_global_entry *)((const UINT8 *)base + SMEM_TOC_OFFSET);
	if (!toc[item].allocated)
		return FALSE;

	offset = toc[item].offset;
	if (!offset || offset + sizeof(*hdr) > smem_size)
		return FALSE;
	if (offset + sizeof(*hdr) > toc[item].size)
		return FALSE;

	hdr = (const struct smp2p_item *)((const UINT8 *)base + offset);
	if (hdr->magic != SMP2P_MAGIC)
		return FALSE;

	valid = hdr->valid_entries;
	if (valid > SMP2P_MAX_ENTRY)
		valid = SMP2P_MAX_ENTRY;
	if (offset + SMP2P_ENTRY_OFFSET + (UINT32)valid * SMP2P_ENTRY_SIZE > smem_size)
		return FALSE;

	for (i = 0; i < valid; i++) {
		const UINT8 *e = (const UINT8 *)hdr + SMP2P_ENTRY_OFFSET + i * SMP2P_ENTRY_SIZE;
		const UINT32 *value = (const UINT32 *)(e + SMP2P_NAME_LEN);

		if (strncmp((const char *)e, "ready", SMP2P_NAME_LEN))
			continue;

		return (*value & 1) ? TRUE : FALSE;
	}

	return FALSE;
}

/* Find the inbound SMP2P item id for @pid from the devicetree, 0 if unknown. */
static UINT32 smp2p_in_item(const void *dtb, UINT16 pid)
{
	int node, len;
	const UINT32 *cells;

	for (node = fdt_next_node(dtb, -1, NULL); node >= 0;
	     node = fdt_next_node(dtb, node, NULL)) {
		if (fdt_node_check_compatible(dtb, node, "qcom,smp2p"))
			continue;

		cells = fdt_getprop(dtb, node, "qcom,remote-pid", &len);
		if (!cells || len < (int)sizeof(UINT32) || fdt32_to_cpu(cells[0]) != pid)
			continue;

		cells = fdt_getprop(dtb, node, "qcom,smem", &len);
		if (!cells || len < (int)(2 * sizeof(UINT32)))
			return 0;

		return fdt32_to_cpu(cells[1]);	/* <outgoing>, <incoming> */
	}

	return 0;
}

EFI_STATUS smp2p_wait_for_remotes(void)
{
	static UINT16 pids[ARRAY_SIZE(rproc_pids)];
	static UINT32 items[ARRAY_SIZE(rproc_pids)];
	UINTN count = 0, i;
	const void *dtb = NULL;
	const void *base;
	UINT32 smem_size = 0;
	UINTN waited;

	if (EFI_ERROR(LibGetSystemConfigurationTable(&EfiDtbTableGuid, (VOID **)&dtb)) || !dtb) {
		qlog("qebspil: SMP2P: no DTB, cannot wait for the remotes\n");
		return EFI_NOT_FOUND;
	}

	base = smem_base(dtb, &smem_size);
	if (!base) {
		qlog("qebspil: SMP2P: no smem node in the DTB\n");
		return EFI_NOT_FOUND;
	}

	/* Only wait for the remotes that were actually started. */
	for (i = 0; count < ARRAY_SIZE(rproc_pids); i++) {
		const struct pil *pil = pil_loaded(i);
		UINT32 item = 0;
		UINTN j;

		if (!pil)
			break;

		for (j = 0; j < ARRAY_SIZE(rproc_pids); j++) {
			if (strcmp(pil->type->compatible, rproc_pids[j].compatible))
				continue;

			item = smp2p_in_item(dtb, rproc_pids[j].pid);
			if (!item)
				break;

			pids[count] = rproc_pids[j].pid;
			items[count] = item;
			count++;
			break;
		}
	}

	if (!count) {
		qlog("qebspil: SMP2P: nothing to wait for\n");
		return EFI_NOT_FOUND;
	}

	for (waited = 0; waited < WAIT_DEADLINE_MS; waited += WAIT_POLL_MS) {
		BOOLEAN all_ready = TRUE;

		for (i = 0; i < count; i++) {
			if (item_ready(base, smem_size, items[i]))
				continue;

			all_ready = FALSE;
			break;
		}

		if (all_ready) {
			for (i = 0; i < count; i++)
				qlog("qebspil: SMP2P: pid %u ready after %u ms\n",
				     pids[i], (UINT32)waited);
			return EFI_SUCCESS;
		}

		BS->Stall(WAIT_POLL_MS * 1000);
	}

	for (i = 0; i < count; i++)
		qlog("qebspil: SMP2P: pid %u not ready after %u ms\n",
		     pids[i], (UINT32)WAIT_DEADLINE_MS);

	return EFI_TIMEOUT;
}
