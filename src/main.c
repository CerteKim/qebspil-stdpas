// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Stephan Gerhold <stephan@gerhold.net>
 */

#include <efi.h>
#include <efilib.h>

#include "qebspil.h"

EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
static EFI_EVENT ebs_event;

/*
 * How long to wait after starting the remoteprocs, before returning from
 * ExitBootServices (after which slbounce performs the Secure Launch switch and
 * takes the hypervisor away).
 *
 * The DSPs need a while to finish their handover with the hypervisor; a remote
 * that is still in the middle of its boot does not survive the switch. That is
 * observed on the Xiaomi Book S 12.4 (sc8180x) as "SMP2P ... -19" in the
 * kernel: the same devicetree + firmware attaches on some boots and not on
 * others, purely depending on how far the DSP got before the switch.
 *
 * FIXME: wait for the SMP2P ready signals instead of a fixed delay.
 */
#ifndef QEBSPIL_SETTLE_MS
#define QEBSPIL_SETTLE_MS 5000
#endif

static EFI_STATUS efi_late_ebs(void)
{
	EFI_STATUS status;

	qlog("qebspil: late ExitBootServices at EL%u\n", current_el());
	status = pil_finish_all();
	qlog("qebspil: pil_finish_all -> %r (EL%u)\n", status, current_el());
	if (status == EFI_NOT_FOUND)
		return EFI_SUCCESS; /* No rempteprocs found */

	/*
	 * Let the remoteprocs settle *before* the switch: slbounce performs the
	 * Secure Launch right after this returns, and whatever the hypervisor
	 * was doing for the DSPs goes away with it. QEBSPIL_SETTLE_MS = 0
	 * restores the old behaviour (return immediately on success).
	 */
	if (QEBSPIL_SETTLE_MS > 0) {
		qlog("qebspil: waiting %u ms for the remoteprocs to settle\n",
		     (UINT32)QEBSPIL_SETTLE_MS);
		BS->Stall((UINTN)QEBSPIL_SETTLE_MS * 1000);
		qlog("qebspil: settle done (EL%u)\n", current_el());
	}

	return status;
}

static EFI_STATUS efi_dtb_changed(void)
{
	EFI_STATUS status;
	void *dtb;

	qlog("qebspil: DTB callback at EL%u\n", current_el());

	/* Free what we prepared last time in case the DTB changed */
	pil_free_all();

	status = LibGetSystemConfigurationTable(&EfiDtbTableGuid, &dtb);
	if (status == EFI_NOT_FOUND)
		return EFI_SUCCESS; /* DTB was removed */
	if (EFI_ERROR(status))
		return status;

	status = dtb_enumerate_rprocs(dtb);
	qlog("qebspil: dtb_enumerate_rprocs -> %r\n", status);
	if (status == EFI_NOT_FOUND)
		return EFI_SUCCESS; /* No remoteprocs found */
	if (EFI_ERROR(status))
		return status;

	/* Register late EBS event (if not already) to start the rprocs later */
	if (!ebs_event) {
		status = event_register_late_ebs_callback(&ebs_event, efi_late_ebs);
		if (EFI_ERROR(status))
			return status;
	}

	return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table)
{
	EFI_STATUS status;
	EFI_EVENT event;

	InitializeLib(image, system_table);

	qlog("Hello World!\n");

	status = BS->HandleProtocol(image, &LoadedImageProtocol, (VOID**)&LoadedImage);
	if (EFI_ERROR(status))
		return status;

	qlog("qebspil: efi_main (built %a %a), device handle %p, EL%u\n",
	     __DATE__, __TIME__, LoadedImage->DeviceHandle, current_el());

	status = scm_init();
	qlog("qebspil: scm_init -> %r\n", status);
	if (EFI_ERROR(status))
		return status;

	status = event_register_group_callback(&EfiDtbTableGuid, &event, efi_dtb_changed);
	if (EFI_ERROR(status))
		return status;

	return efi_dtb_changed();
}
