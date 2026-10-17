// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Stephan Gerhold <stephan@gerhold.net>
 */

#include <efi.h>
#include <efilib.h>

#include "qebspil.h"
#include "external/pil-proxy-protocol.h"

#define MAX_PIL_NUM		4	/* SC8280XP */

static struct pil loaded_pil[MAX_PIL_NUM];

static const char *const pil_component_names[PIL_COMPONENTS] = {
	" DTB",
	"",
};

struct pil *pil_alloc(const struct pil_type *type)
{
	for (UINTN i = 0; i < ARRAY_SIZE(loaded_pil); i++) {
		if (loaded_pil[i].type) {
			if (loaded_pil[i].type == type)
				return NULL;
			continue;
		}

		loaded_pil[i].type = type;
		return &loaded_pil[i];
	}

	return NULL;
}

EFI_STATUS pil_proxy_vote(const EFI_GUID *guid)
{
	PIL_PROXY_PROTOCOL *proxy;
	EFI_STATUS status;

	if (!guid)
		return EFI_INVALID_PARAMETER;

	status = LibLocateProtocol((EFI_GUID*)guid, (VOID**)&proxy);
	if (EFI_ERROR(status))
		return status;

	return proxy->Vote(PIL_PROXY_MODE_VOTE);
}

EFI_STATUS pil_prepare(struct pil *pil)
{
	/* Make sure firmware was loaded for all components */
	for (enum pil_component c = 0; c < PIL_COMPONENTS; c++) {
		if (!pil_has_component(pil, c))
			continue;

		if (!pil->fw[c].elf_data)
			return EFI_LOAD_ERROR;

		/*
		 * TODO: Perhaps we should parse the EFI memory map and ensure
		 * that the memory regions are reserved?
		 */
	}

	/*
	 * Ideally we'd make this vote right before we boot the remoteproc and
	 * drop it right after. Unfortunately since we run in EBS, the driver
	 * that provides us with the convenient proxy vote protocol will likely
	 * be shutting down so we can't really rely on it being available then.
	 *
	 * Thus we just vote early and hope the OS will drop the vote later.
	 *
	 * The standard PAS backend does not go through the firmware's PIL
	 * proxy at all, so skip the vote there instead of failing on a
	 * protocol the firmware may not provide.
	 */
#ifndef QEBSPIL_STD_PAS
	if (!IsZeroGuid(&pil->type->proxy_guid)) {
		EFI_STATUS status = pil_proxy_vote(&pil->type->proxy_guid);

		if (EFI_ERROR(status)) {
			qlog("qebspil: Proxy vote failed for %a: %r\n",
			      pil->type->compatible, status);
			return status;
		}
		qlog("qebspil: proxy vote ok for %a\n", pil->type->compatible);
	}
#endif

	/*
	 * We could move fw_check() and fw_load_metadata() here, but for now
	 * we just do it all together in pil_finish() below (on late EBS).
	 */

	return EFI_SUCCESS;
}

static EFI_STATUS pil_finish(struct pil *pil)
{
	EFI_STATUS status;

	qlog("qebspil: Starting remoteproc: %a\n", pil->type->compatible);

	for (enum pil_component c = 0; c < PIL_COMPONENTS; c++) {
		if (!pil_has_component(pil, c))
			continue;

		status = fw_check(&pil->fw[c]);
		if (EFI_ERROR(status)) {
			qlog("qebspil: Firmware check failed for %a%a: %r\n",
			      pil->type->compatible, pil_component_names[c], status);
			return status;
		}
	}

	for (enum pil_component c = 0; c < PIL_COMPONENTS; c++) {
		if (!pil_has_component(pil, c))
			continue;

		status = fw_load_metadata(&pil->fw[c]);
		if (EFI_ERROR(status)) {
			qlog("qebspil: Failed to load firmware metadata for %a%a: %r\n",
			      pil->type->compatible, pil_component_names[c], status);
			return status;
		}
	}

	for (enum pil_component c = 0; c < PIL_COMPONENTS; c++) {
		const struct pil_fw *fw = &pil->fw[c];

		if (!pil_has_component(pil, c))
			continue;

		status = scm_pil_init(pil->type->id[c].full, fw->metadata);
		if (EFI_ERROR(status)) {
			qlog("qebspil: Failed to init firmware for %a%a: %r (wrong firmware?)\n",
			      pil->type->compatible, pil_component_names[c], status);
			return status;
		}

		status = scm_pil_mem_setup(pil->type->id[c].full, fw->mem_addr, fw->mem_size);
		if (EFI_ERROR(status)) {
			qlog("qebspil: Failed to setup memory area for %a%a: %r\n",
			      pil->type->compatible, pil_component_names[c], status);
			return status;
		}
	}

	for (enum pil_component c = 0; c < PIL_COMPONENTS; c++) {
		if (!pil_has_component(pil, c))
			continue;

		status = fw_load(&pil->fw[c]);
		if (EFI_ERROR(status)) {
			qlog("qebspil: Failed to load firmware for %a%a: %r\n",
			      pil->type->compatible, pil_component_names[c], status);
			return status;
		}
	}

	/* Stop lite firmware in reverse order (full first, then DTB) */
	for (enum pil_component b = PIL_COMPONENTS; b > 0; b--) {
		if (!pil_has_component(pil, b - 1) || !pil->type->id[b - 1].lite)
			continue;

		/* Ignore return status since we don't know if lite was running */
		scm_pil_stop(pil->type->id[b - 1].lite);
	}

	for (enum pil_component c = 0; c < PIL_COMPONENTS; c++) {
		if (!pil_has_component(pil, c))
			continue;

		status = scm_pil_start(pil->type->id[c].full);
		if (EFI_ERROR(status)) {
			qlog("qebspil: Failed to authenticate and start firmware for %a%a: %r\n",
			      pil->type->compatible, pil_component_names[c], status);

			/* Rollback */
			for (enum pil_component b = c; b > 0; b--)
				if (pil_has_component(pil, b - 1))
					scm_pil_stop(pil->type->id[b - 1].full);

			return status;
		}
	}

	qlog("qebspil: started %a\n", pil->type->compatible);
	return EFI_SUCCESS;
}

const struct pil *pil_loaded(UINTN index)
{
	if (index >= ARRAY_SIZE(loaded_pil) || !loaded_pil[index].type)
		return NULL;

	return &loaded_pil[index];
}

EFI_STATUS pil_finish_all(void)
{
	EFI_STATUS found = EFI_NOT_FOUND;
	EFI_STATUS status;

	for (UINTN i = 0; i < ARRAY_SIZE(loaded_pil); i++) {
		if (!loaded_pil[i].type)
			break;

		status = pil_finish(&loaded_pil[i]);
		if (!EFI_ERROR(status))
			found = EFI_SUCCESS;
	}

	return found;
}

void pil_free(struct pil *pil)
{
	for (enum pil_component c = 0; c < PIL_COMPONENTS; c++)
		fw_free(&pil->fw[c]);
	ZeroMem(pil, sizeof(*pil));
}

void pil_free_all(void)
{
	for (UINTN i = 0; i < ARRAY_SIZE(loaded_pil); i++)
		pil_free(&loaded_pil[i]);
}
