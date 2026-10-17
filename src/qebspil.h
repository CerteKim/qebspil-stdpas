/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QEBSPIL_H
#define QEBSPIL_H

#include <efi.h>

#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))

enum pil_component {
	PIL_DTB,
	PIL_MAIN,
	PIL_COMPONENTS,
};

struct pil_id {
	UINT8 full;
	UINT8 lite;
};

struct pil_type {
	const char *compatible;
	struct pil_id id[PIL_COMPONENTS];
	EFI_GUID proxy_guid;
};

struct pil_fw {
	VOID *elf_data;
	UINTN elf_size;
	EFI_PHYSICAL_ADDRESS metadata;
	UINT32 mem_addr;
	UINT32 mem_size;
};

struct pil {
	const struct pil_type *type;
	struct pil_fw fw[PIL_COMPONENTS];
};

extern EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;

void qlog(const char *fmt, ...);

/* Are we below or above the hypervisor? Useful to see when slbounce switched. */
static inline unsigned int current_el(void)
{
	unsigned long el;

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	return (unsigned int)((el >> 2) & 3);
}

EFI_STATUS dtb_enumerate_rprocs(const void *dtb);

typedef EFI_STATUS (*event_callback_fn)(void);
EFI_STATUS event_register_group_callback(const EFI_GUID *group, EFI_EVENT *event, event_callback_fn fn);
EFI_STATUS event_register_late_ebs_callback(EFI_EVENT *event, event_callback_fn fn);

EFI_STATUS fw_prepare(struct pil_fw *fw, EFI_FILE_HANDLE root, const char *fw_name);
EFI_STATUS fw_check(const struct pil_fw *fw);
EFI_STATUS fw_load_metadata(struct pil_fw *fw);
EFI_STATUS fw_load(const struct pil_fw *fw);
void fw_free(struct pil_fw *fw);

static inline BOOLEAN pil_has_component(const struct pil *pil, enum pil_component c)
{
	return !!pil->type->id[c].full;
}

struct pil *pil_alloc(const struct pil_type *type);
EFI_STATUS pil_prepare(struct pil *pil);
EFI_STATUS pil_finish_all(void);
const struct pil *pil_loaded(UINTN index);
void pil_free(struct pil *pil);
void pil_free_all(void);

const struct pil_type *pil_types_find(const char *compatible);

EFI_STATUS smp2p_wait_for_remotes(void);

EFI_STATUS scm_init(void);
EFI_STATUS scm_pil_init(UINT8 pas_id, EFI_PHYSICAL_ADDRESS metadata);
EFI_STATUS scm_pil_mem_setup(UINT8 pas_id, EFI_PHYSICAL_ADDRESS addr, UINTN size);
EFI_STATUS scm_pil_start(UINT8 pas_id);
EFI_STATUS scm_pil_stop(UINT8 pas_id);

#endif
