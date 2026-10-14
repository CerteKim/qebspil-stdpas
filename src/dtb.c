// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Stephan Gerhold <stephan@gerhold.net>
 */

#include <efi.h>
#include <efilib.h>

#include <libfdt.h>

#include "external/lkfdt.h"
#include "qebspil.h"

struct dtb_context {
	const void *const dtb;
	int rmem_node;
	EFI_FILE_HANDLE root_dir;
};

static EFI_STATUS dtb_load_pil_data(struct dtb_context *ctx, int node, struct pil_fw *fw, int idx)
{
	uint32_t mem_phandle;
	const char *fw_name;
	int mem_node;
	int ret;

	ret = lkfdt_u32list_get(ctx->dtb, node, "memory-region", idx, &mem_phandle);
	if (ret)
		return EFI_INVALID_PARAMETER;

	mem_node = lkfdt_subnode_offset_by_phandle(ctx->dtb, ctx->rmem_node, mem_phandle);
	if (mem_node < 0)
		return EFI_INVALID_PARAMETER;

	if (!lkfdt_node_is_available(ctx->dtb, mem_node))
		return EFI_INVALID_PARAMETER;

	ret = lkfdt_get_reg(ctx->dtb, ctx->rmem_node, mem_node, &fw->mem_addr, &fw->mem_size);
	if (ret)
		return EFI_INVALID_PARAMETER;

	fw_name = fdt_stringlist_get(ctx->dtb, node, "firmware-name", idx, NULL);
	if (!fw_name)
		return EFI_INVALID_PARAMETER;

	qlog("Firmware %d: base 0x%x, size 0x%x, path %a\n", idx, fw->mem_addr, fw->mem_size, fw_name);
	return fw_prepare(fw, ctx->root_dir, fw_name);
}

static EFI_STATUS dtb_enumerate_rproc(struct dtb_context *ctx, int node, struct pil *pil)
{
	EFI_STATUS status;

	for (enum pil_component c = 0; c < PIL_COMPONENTS; c++) {
		/* DTB comes first in enum pil_component but last in DTB */
		int idx = PIL_COMPONENTS - c - 1;

		if (!pil_has_component(pil, c))
			continue;

		status = dtb_load_pil_data(ctx, node, &pil->fw[c], idx);
		if (EFI_ERROR(status))
			return status;
	}

	return EFI_SUCCESS;
}

static bool dtb_init_ctx(struct dtb_context *ctx)
{
	if (!ctx->rmem_node) {
		ctx->rmem_node = fdt_path_offset(ctx->dtb, "/reserved-memory");
		if (ctx->rmem_node < 0) {
			qlog("qebspil: Failed to find /reserved-memory in DTB: %d\n", ctx->rmem_node);
			return false;
		}
	}

	if (!ctx->root_dir) {
		ctx->root_dir = LibOpenRoot(LoadedImage->DeviceHandle);
		if (!ctx->root_dir) {
			qlog("qebspil: Failed to open root volume. Not started from file?\n");
			return false;
		}
	}

	return true;
}

static void dtb_free_ctx(struct dtb_context *ctx)
{
	if (ctx->root_dir)
		ctx->root_dir->Close(ctx->root_dir);
}

static const struct pil_type *dtb_find_pil_type(const void *dtb, int node)
{
	const char *compatible;
	const char *end;
	int len;

	compatible = fdt_getprop(dtb, node, "compatible", &len);
	if (!compatible || len <= 0 || compatible[len - 1])
		return NULL;

	end = compatible + len;
	for (; compatible < end; compatible += strlen(compatible) + 1) {
		const struct pil_type *type = pil_types_find(compatible);

		if (type)
			return type;
	}

	return NULL;
}

EFI_STATUS dtb_enumerate_rprocs(const void *dtb)
{
	EFI_STATUS found = EFI_NOT_FOUND;
	struct dtb_context ctx = {
		.dtb = dtb,
	};
	int node = -1;
	int ret;

	ret = fdt_check_header(dtb);
	if (ret) {
		qlog("qebspil: bad DTB header: %d\n", ret);
		return EFI_VOLUME_CORRUPTED;
	}

	while ((node = fdt_next_node(dtb, node, NULL)) >= 0) {
		const struct pil_type *type;
		EFI_STATUS status;
		const char *name;
		struct pil *pil;

		name = fdt_get_name(dtb, node, NULL);
		if (!name || strncmp(name, "remoteproc", sizeof("remoteproc") - 1))
			continue;

		if (!lkfdt_node_is_available(dtb, node))
			continue;

		if (!QEBSPIL_ALWAYS_START && !fdt_getprop(dtb, node, "qcom,broken-reset", NULL))
			continue;

		type = dtb_find_pil_type(dtb, node);
		if (!type)
			continue;

		qlog("qebspil: Found remoteproc: %a\n", type->compatible);

		if (!dtb_init_ctx(&ctx))
			return found;

		pil = pil_alloc(type);
		if (!pil) {
			qlog("Failed to allocate remoteproc %a\n", type->compatible);
			continue;
		}

		status = dtb_enumerate_rproc(&ctx, node, pil);
		if (EFI_ERROR(status)) {
			qlog("Failed to enumerate remoteproc %a: %r\n", type->compatible, status);
			pil_free(pil);
			continue;
		}

		status = pil_prepare(pil);
		if (EFI_ERROR(status)) {
			qlog("Failed to prepare remoteproc PIL %a: %r\n", type->compatible, status);
			pil_free(pil);
			continue;
		}

		found = EFI_SUCCESS;
	}

	dtb_free_ctx(&ctx);
	qlog("qebspil: rprocs enumerated, result %r\n", found);
	return found;
}
