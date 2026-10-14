// SPDX-License-Identifier: GPL-2.0-only

#include <string.h>

#include "qebspil.h"
#include "external/pil-proxy-protocol.h"

static const struct pil_type pil_types[] = {
	{
		.compatible = "qcom,sc7180-adsp-pas",
		.id[PIL_MAIN].full = 1
	},

	/*
	 * Xiaomi Book S 12.4 (SC8180X). The DSPs are owned by the firmware and
	 * Linux cannot reset them at EL2, so they have to be started here (while
	 * the firmware services are still available) and then attached to.
	 * PAS ids match the kernel's qcom_q6v5_pas resources.
	 */
	{
		.compatible = "qcom,sc8180x-adsp-pas",
		.id[PIL_MAIN].full = 1,
		.proxy_guid = PIL_PROXY_ADSP_GUID,
	},
	{
		.compatible = "qcom,sc8180x-cdsp-pas",
		.id[PIL_MAIN].full = 18,
		.proxy_guid = PIL_PROXY_CDSP_GUID,
	},
	{
		.compatible = "qcom,sc8180x-slpi-pas",
		.id[PIL_MAIN].full = 12,
		.proxy_guid = PIL_PROXY_SLPI_GUID,
	},

	{
		.compatible = "qcom,sc8280xp-adsp-pas",
		.id[PIL_MAIN].full = 1,
		.proxy_guid = PIL_PROXY_ADSP_GUID,
	},
	{
		.compatible = "qcom,sc8280xp-slpi-pas",
		.id[PIL_MAIN].full = 12,
		.proxy_guid = PIL_PROXY_SLPI_GUID,
	},
	{
		.compatible = "qcom,sc8280xp-nsp0-pas",
		.id[PIL_MAIN].full = 18,
		.proxy_guid = PIL_PROXY_CDSP_GUID,
	},
	{
		.compatible = "qcom,sc8280xp-nsp1-pas",
		.id[PIL_MAIN].full = 30,
		.proxy_guid = PIL_PROXY_CDSP1_GUID,
	},

	{
		.compatible = "qcom,x1e80100-adsp-pas",
		.id[PIL_MAIN] = {
			.full = 1,
			.lite = 31,
		},
		.id[PIL_DTB] = {
			.full = 36,
			.lite = 41,
		},
		.proxy_guid = PIL_PROXY_ADSP_DTB_GUID,
	},
	{
		.compatible = "qcom,x1e80100-cdsp-pas",
		.id[PIL_MAIN].full = 18,
		.id[PIL_DTB].full = 37,
		.proxy_guid = PIL_PROXY_CDSP_DTB_GUID,
	},
};

const struct pil_type *pil_types_find(const char *compatible)
{
	for (size_t i = 0; i < ARRAY_SIZE(pil_types); i++)
		if (strcmp(compatible, pil_types[i].compatible) == 0)
			return &pil_types[i];

	return NULL;
}
