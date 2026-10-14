// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Stephan Gerhold <stephan@gerhold.net>
 *
 * Two ways to ask the secure world to boot a remoteproc:
 *
 *  - Default: the QcomScmProtocol provided by Qualcomm's ABL firmware, i.e.
 *    the TZ_PIL_* SIP functions (upstream qebspil behaviour).
 *
 *  - QEBSPIL_STD_PAS: the standard QCOM_SCM_SVC_PIL interface that the Linux
 *    kernel uses, issued as raw SMC calls. On machines whose firmware
 *    implements the actual reset sequence in the hypervisor, the ABL
 *    interface can report success without ever releasing the remoteproc from
 *    reset -- while the interface the OS uses is the one known to work at
 *    EL1, i.e. while the hypervisor is still alive.
 */

#include <efi.h>
#include <efilib.h>

#include "qebspil.h"

#ifdef QEBSPIL_STD_PAS

#include "external/pil-proxy-protocol.h"

/* include/linux/firmware/qcom/qcom_scm.h + drivers/firmware/qcom/qcom_scm.h */
#define QCOM_SCM_SVC_PIL		0x02
#define QCOM_SCM_PIL_PAS_INIT_IMAGE	0x01
#define QCOM_SCM_PIL_PAS_MEM_SETUP	0x02
#define QCOM_SCM_PIL_PAS_AUTH_AND_RESET	0x05
#define QCOM_SCM_PIL_PAS_SHUTDOWN	0x06

#define QCOM_SCM_VAL			0
#define QCOM_SCM_RW			2

/* QCOM_SCM_ARGS(num, type0, type1, ...): num | type0 << 4 | type1 << 6 | ... */
#define QCOM_SCM_ARGS_2_VAL_RW		(2 | (QCOM_SCM_VAL << 4) | (QCOM_SCM_RW << 6))
#define QCOM_SCM_ARGS_3_VAL		(3)
#define QCOM_SCM_ARGS_1_VAL		(1)

/* drivers/firmware/qcom/qcom_scm-smc.c */
#define ARM_SMCCC_OWNER_SIP		2
#define SCM_SMC_FNID(s, c)		((((s) & 0xff) << 8) | ((c) & 0xff))
#define SCM_SMCCC_FNID(s, c) \
	(((UINT64)1 << 30) | ((UINT64)ARM_SMCCC_OWNER_SIP << 24) | SCM_SMC_FNID(s, c))

#define QCOM_SCM_INTERRUPTED		1
#define QCOM_SCM_V2_EBUSY		((UINT64)-12)

/*
 * smc64 with the "std call" flag (bit 31 clear), owner SIP, like the kernel
 * does for the non-atomic path. x0..x5 carry {fn, arginfo, arg0..arg3}; the
 * secure world returns {status, result0, result1, result2} in x0..x3.
 */
static void scm_smc(UINT64 fn, UINT64 arginfo, UINT64 a0, UINT64 a1, UINT64 a2,
		    UINT64 res[4])
{
	register UINT64 x0 __asm__("x0") = fn;
	register UINT64 x1 __asm__("x1") = arginfo;
	register UINT64 x2 __asm__("x2") = a0;
	register UINT64 x3 __asm__("x3") = a1;
	register UINT64 x4 __asm__("x4") = a2;

	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4)
			 :
			 : "memory");

	res[0] = x0;
	res[1] = x1;
	res[2] = x2;
	res[3] = x3;
}

static EFI_STATUS scm_pas_call(const char *name, UINT32 cmd, UINT32 arginfo,
			       UINT64 a0, UINT64 a1, UINT64 a2,
			       BOOLEAN check_result0)
{
	UINT64 res[4] = { 0 };
	UINTN interrupted = 0;
	UINTN busy = 0;

	for (;;) {
		scm_smc(SCM_SMCCC_FNID(QCOM_SCM_SVC_PIL, cmd), arginfo, a0, a1, a2, res);

		if (res[0] == QCOM_SCM_INTERRUPTED) {
			/* Std calls can be interrupted; re-issue the same call */
			if (++interrupted < 100)
				continue;
		} else if (res[0] == QCOM_SCM_V2_EBUSY) {
			if (++busy < 20) {
				BS->Stall(30 * 1000);
				continue;
			}
		}

		break;
	}

	qlog("qebspil: %a(pas_id=%u, 0x%lx, 0x%lx) -> a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx\n",
	     name, (UINT32)a0, a1, a2, res[0], res[1], res[2], res[3]);

	if (res[0] != 0 || (check_result0 && res[1] != 0)) {
		qlog("qebspil: %a failed (status 0x%lx / 0x%lx)\n", name, res[0], res[1]);
		return EFI_DEVICE_ERROR;
	}

	return EFI_SUCCESS;
}

#if QEBSPIL_DUMP_PROTOCOLS
static const char *scm_pil_proxy_name(const EFI_GUID *guid)
{
	static const struct {
		const char *name;
		EFI_GUID guid;
	} known[] = {
		{ "PIL_PROXY_ADSP", PIL_PROXY_ADSP_GUID },
		{ "PIL_PROXY_MODEM", PIL_PROXY_MODEM_GUID },
		{ "PIL_PROXY_WPSS", PIL_PROXY_WPSS_GUID },
		{ "PIL_PROXY_SLPI", PIL_PROXY_SLPI_GUID },
		{ "PIL_PROXY_CDSP", PIL_PROXY_CDSP_GUID },
		{ "PIL_PROXY_UNK23", PIL_PROXY_UNK23_GUID },
		{ "PIL_PROXY_CDSP1", PIL_PROXY_CDSP1_GUID },
		{ "PIL_PROXY_ADSP_DTB", PIL_PROXY_ADSP_DTB_GUID },
		{ "PIL_PROXY_CDSP_DTB", PIL_PROXY_CDSP_DTB_GUID },
		{ "PIL_PROXY_UNK38", PIL_PROXY_UNK38_GUID },
	};

	for (UINTN i = 0; i < ARRAY_SIZE(known); i++)
		if (!CompareGuid((EFI_GUID *)&known[i].guid, (EFI_GUID *)guid))
			return known[i].name;

	return NULL;
}

/*
 * Dump every installed EFI protocol GUID. This tells us whether this
 * firmware publishes PilProxyDxe protocols at all (and under which GUIDs),
 * which is what the ABL backend needs for its RPMh proxy votes.
 */
static void scm_dump_protocols(void)
{
	EFI_HANDLE *handles = NULL;
	UINTN nhandles = 0;
	UINTN count = 0;
	EFI_STATUS status;

	status = LibLocateHandle(AllHandles, NULL, NULL, &nhandles, &handles);
	qlog("qebspil: protocol dump: %u handles (%r)\n", nhandles, status);
	if (EFI_ERROR(status))
		return;

	for (UINTN i = 0; i < nhandles && count < 2000; i++) {
		EFI_GUID **guids = NULL;
		UINTN nguids = 0;

		status = BS->ProtocolsPerHandle(handles[i], &guids, &nguids);
		if (EFI_ERROR(status))
			continue;

		for (UINTN j = 0; j < nguids && count < 2000; j++, count++) {
			const char *name = scm_pil_proxy_name(guids[j]);

			qlog("qebspil: protocol: %g%s%a (handle %p)\n", guids[j],
			     name ? " <== " : "", name ? name : "", handles[i]);
		}

		FreePool(guids);
	}

	FreePool(handles);
	qlog("qebspil: protocol dump done, %u GUIDs\n", count);
}
#endif /* QEBSPIL_DUMP_PROTOCOLS */

EFI_STATUS scm_init(void)
{
	qlog("qebspil: using the standard QCOM_SCM_SVC_PIL backend at EL%u\n",
	     current_el());

#if QEBSPIL_DUMP_PROTOCOLS
	scm_dump_protocols();
#endif

	return EFI_SUCCESS;
}

EFI_STATUS scm_pil_init(UINT8 pas_id, EFI_PHYSICAL_ADDRESS metadata)
{
	return scm_pas_call("PAS_INIT_IMAGE", QCOM_SCM_PIL_PAS_INIT_IMAGE,
			    QCOM_SCM_ARGS_2_VAL_RW, pas_id, metadata, 0, FALSE);
}

EFI_STATUS scm_pil_mem_setup(UINT8 pas_id, EFI_PHYSICAL_ADDRESS addr, UINTN size)
{
	return scm_pas_call("PAS_MEM_SETUP", QCOM_SCM_PIL_PAS_MEM_SETUP,
			    QCOM_SCM_ARGS_3_VAL, pas_id, addr, size, TRUE);
}

EFI_STATUS scm_pil_start(UINT8 pas_id)
{
	return scm_pas_call("PAS_AUTH_AND_RESET", QCOM_SCM_PIL_PAS_AUTH_AND_RESET,
			    QCOM_SCM_ARGS_1_VAL, pas_id, 0, 0, TRUE);
}

EFI_STATUS scm_pil_stop(UINT8 pas_id)
{
	return scm_pas_call("PAS_SHUTDOWN", QCOM_SCM_PIL_PAS_SHUTDOWN,
			    QCOM_SCM_ARGS_1_VAL, pas_id, 0, 0, TRUE);
}

#else /* !QEBSPIL_STD_PAS */

#include "external/EFIScm.h"
#include "external/scm_sip_interface.h"

static QCOM_SCM_PROTOCOL *scm;

// https://git.codelinaro.org/clo/la/abl/tianocore/edk2/-/blob/72c4d886ce585d584f2e3eeb87fd3761819824e7/QcomModulePkg/QcomModulePkg.dec#L102
EFI_GUID gQcomScmProtocolGuid = { 0x77ed108d, 0x8524, 0x4b8b, { 0x9d, 0x2e, 0x34, 0x98, 0x7a, 0xec, 0xb9, 0xc1 } };

EFI_STATUS scm_init(void)
{
	EFI_STATUS status;

	status = LibLocateProtocol(&gQcomScmProtocolGuid, (VOID**)&scm);
	if (EFI_ERROR(status)) {
		qlog("qebspil: Failed to find QCOM SCM protocol: %r\n", status);
		return status;
	}

	qlog("qebspil: Found QCOM SCM protocol version 0x%x\n", scm->Revision);
	return EFI_SUCCESS;
}

EFI_STATUS scm_pil_init(UINT8 pas_id, EFI_PHYSICAL_ADDRESS metadata)
{
	UINT64 Parameters[SCM_MAX_NUM_PARAMETERS] = {
		pas_id,
		metadata,
	};
	UINT64 Results[SCM_MAX_NUM_RESULTS];

	return scm->ScmSipSysCall(scm, TZ_PIL_INIT_ID, TZ_PIL_INIT_ID_PARAM_ID, Parameters, Results);
}

EFI_STATUS scm_pil_mem_setup(UINT8 pas_id, EFI_PHYSICAL_ADDRESS addr, UINTN size)
{
	UINT64 Parameters[SCM_MAX_NUM_PARAMETERS] = {
		pas_id,
		addr,
		size,
	};
	UINT64 Results[SCM_MAX_NUM_RESULTS];

	return scm->ScmSipSysCall(scm, TZ_PIL_MEM_ID, TZ_PIL_MEM_ID_PARAM_ID, Parameters, Results);
}

EFI_STATUS scm_pil_start(UINT8 pas_id)
{
	UINT64 Parameters[SCM_MAX_NUM_PARAMETERS] = {
		pas_id,
	};
	UINT64 Results[SCM_MAX_NUM_RESULTS];

	return scm->ScmSipSysCall(scm, TZ_PIL_AUTH_RESET_ID, TZ_PIL_AUTH_RESET_ID_PARAM_ID, Parameters, Results);
}

EFI_STATUS scm_pil_stop(UINT8 pas_id)
{
	UINT64 Parameters[SCM_MAX_NUM_PARAMETERS] = {
		pas_id,
	};
	UINT64 Results[SCM_MAX_NUM_RESULTS];

	return scm->ScmSipSysCall(scm, TZ_PIL_UNLOCK_XPU_ID, TZ_PIL_UNLOCK_XPU_ID_PARAM_ID, Parameters, Results);
}

#endif /* QEBSPIL_STD_PAS */
