# SPDX-License-Identifier: GPL-2.0-only
# Based on external/gnu-efi/apps/Makefile

ARCH 		:= aarch64
CROSS_COMPILE 	:=

SRCDIR		:= $(CURDIR)
OUTDIR		:= $(CURDIR)/out

GNUEFI_DIR	:= $(SRCDIR)/external/gnu-efi
GNUEFI_OUT	:= $(GNUEFI_DIR)/$(ARCH)

LIBFDT_DIR	:= external/dtc/libfdt

export TOPDIR	?= $(GNUEFI_DIR)
include $(GNUEFI_DIR)/Make.defaults

CRTOBJS		= $(OBJDIR)/gnuefi/crt0-efi-$(ARCH).o
LDSCRIPT	= $(GNUEFI_DIR)/gnuefi/elf_$(ARCH)_efi.lds

EFI_LDFLAGS	+= -L$(GNUEFI_OUT)/lib -L$(GNUEFI_OUT)/gnuefi $(CRTOBJS)
EFI_LIBS	+= -T $(LDSCRIPT)

EFI_LDFLAGS	+= -pie -Bsymbolic -gc-sections

EFI_CFLAGS      += -fPIE -ffunction-sections -fdata-sections
EFI_LDFLAGS     += --no-dynamic-linker

EFI_LIBS	+= -lefi -lgnuefi

FORMAT		:= -O efi-bsdrv-$(ARCH)

QEBSPIL_ALWAYS_START	?= 0
EFI_CFLAGS		+= -DQEBSPIL_ALWAYS_START=$(QEBSPIL_ALWAYS_START)

# SCM backend: "abl" = QcomScmProtocol / TZ_PIL_* (upstream qebspil behaviour),
# "std" = the standard QCOM_SCM_SVC_PIL interface the Linux kernel uses.
SCM_BACKEND	?= abl
ifeq ($(SCM_BACKEND),std)
EFI_CFLAGS	+= -DQEBSPIL_STD_PAS=1
EFI_NAME	:= qebspilaa64-stdpas.efi
else
EFI_NAME	:= qebspilaa64.efi
endif

# Log the GUID of every installed EFI protocol (diagnostics for the ABL path).
QEBSPIL_DUMP_PROTOCOLS	?= 0
EFI_CFLAGS		+= -DQEBSPIL_DUMP_PROTOCOLS=$(QEBSPIL_DUMP_PROTOCOLS)

QEBSPIL_OBJS := \
	src/dtb.o \
	src/event.o \
	src/fw.o \
	src/log.o \
	src/main.o \
	src/pil.o \
	src/pil-types.o \
	src/scm.o \
	src/external/cache.o \
	src/external/libc.o \
	src/external/lkfdt.o \

.PHONY: all
all: $(OUTDIR)/$(EFI_NAME)

include $(CURDIR)/$(LIBFDT_DIR)/Makefile.libfdt
QEBSPIL_OBJS += $(addprefix $(LIBFDT_DIR)/,$(LIBFDT_OBJS))
INCDIR += -I$(CURDIR)/$(LIBFDT_DIR)
EFI_CFLAGS += -D_POSIX_C_SOURCE=200809L
$(OUTDIR)/$(LIBFDT_DIR)/%.o: CFLAGS := $(CFLAGS) -Wno-unused-parameter

include $(GNUEFI_DIR)/Make.rules

$(CRTOBJS):
	@$(MAKE) -C$(GNUEFI_DIR) CROSS_COMPILE=$(CROSS_COMPILE) \
		CFLAGS="$(USER_CFLAGS) -ffunction-sections -fdata-sections" \
		lib gnuefi

$(OUTDIR)/%.o: %.c
	@$(ECHO) "  CC       $(subst $(OUTDIR)/,,$@)"
	@mkdir -p $(dir $@)
	$(HIDE)$(CC) $(INCDIR) $(CFLAGS) -c $< -o $@

$(OUTDIR)/%.so: $(addprefix $(OUTDIR)/,$(QEBSPIL_OBJS)) | $(CRTOBJS)
	@$(ECHO) "  LD       $(notdir $@)"
	$(HIDE)$(LD) $(LDFLAGS) $^ -o $@ $(EFI_LIBS)

.PHONY: clean
clean:
	rm -rf $(OUTDIR)
	$(MAKE) -C$(GNUEFI_DIR) ARCH=$(ARCH) clean
