/*
 * Copyright (c) 2017 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/**
 * @file
 * The PCC and ADC charge record structure allows for the definition of CDR
 * fields with values to be populated by associated values or callback
 * functions. The design allows for common reporting between ADC and PCC rules,
 * and allows for fields to be easily modified, added or removed as needed
 * while maintaining consistency between column headers and their data.
 */

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>

#include <rte_ether.h>
#include <rte_debug.h>

#include "cdr.h"
#include "util.h"

#ifdef SDN_ODL_BUILD
#include "zmqsub.h"
#endif

#define PANIC_ON_UNDEFINED_RULE() \
	rte_panic("Error in %s: CDR must indicate either PCC or ADC rule\n",\
			__FILE__)


char *cdr_path = DEFAULT_CDR_PATH;
FILE *cdr_file;
FILE *cdr_file_stream;
char *cdr_file_stream_ptr;
size_t cdr_file_stream_sizeloc;

uint64_t cdr_count;



/* CDR IP to string helper functions */
const char *
iptoa(struct ip_addr addr)
{
	static char buffer[40];
	switch (addr.iptype) {
	case IPTYPE_IPV4:
		snprintf(buffer, sizeof(buffer), IPV4_ADDR,
				IPV4_ADDR_HOST_FORMAT(addr.u.ipv4_addr));
		break;
	case IPTYPE_IPV6:
		strcpy(buffer, "TODO");
		break;
	default:
		strcpy(buffer, "Invalid IP");
		break;
	}
	return buffer;
}

static const char *
iptoa_prefix(struct ip_addr addr, uint16_t prefix)
{
	static char buffer[40];
	switch (addr.iptype) {
	case IPTYPE_IPV4:
		snprintf(buffer, sizeof(buffer), IPV4_ADDR"/%u",
				IPV4_ADDR_HOST_FORMAT(addr.u.ipv4_addr),
				prefix);
		break;
	case IPTYPE_IPV6:
		strcpy(buffer, "TODO");
		break;
	default:
		strcpy(buffer, "Invalid IP");
		break;
	}
	return buffer;
}


/* begin static cdr callback fucntions */

static const char *
cdr_time_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	static char time_str[RECORD_TIME_LENGTH];
	time_t t = time(NULL);
	struct tm *tmp = localtime(&t);
	if (tmp == NULL)
		return NULL;
	strftime(time_str, RECORD_TIME_LENGTH, RECORD_TIME_FORMAT, tmp);
	return time_str;
}

static const char *
ue_ip_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	return iptoa(session->ue_addr);
}

static uint64_t
dl_pkt_cnt_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	return vol->dl_cdr.pkt_count;
}

static uint64_t
dl_byptes_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	return vol->dl_cdr.bytes;
}

static uint64_t
ul_pkt_cnt_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	return vol->ul_cdr.pkt_count;
}

static uint64_t
ul_byptes_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	return vol->ul_cdr.bytes;
}

static uint32_t
rule_id_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	if (pcc_rule)
		return pcc_rule->rule_id;
	else if (adc_rule)
		return adc_rule->rule_id;
	PANIC_ON_UNDEFINED_RULE();
	return 0;
}

static const char *
rule_type_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	if (pcc_rule)
		return "PCC";
	else if (adc_rule)
		return "ADC";
	PANIC_ON_UNDEFINED_RULE();
	return NULL;
}

static const char *
rule_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	if (pcc_rule)
		return pcc_rule->rule_name;
	else if (adc_rule)
		switch (adc_rule->sel_type) {
		case DOMAIN_IP_ADDR:
			return iptoa(adc_rule->u.domain_ip);
		case DOMAIN_IP_ADDR_PREFIX:
			return iptoa_prefix(adc_rule->u.domain_prefix.ip_addr,
					adc_rule->u.domain_prefix.prefix);
		case DOMAIN_NAME:
			return adc_rule->u.domain_name;
		};
	PANIC_ON_UNDEFINED_RULE();
	return NULL;
}

static const char *
action_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	uint8_t gate_status = UINT8_MAX;
	if (pcc_rule)
		gate_status = pcc_rule->gate_status;
	else if (adc_rule)
		gate_status = adc_rule->gate_status;
	else
		PANIC_ON_UNDEFINED_RULE();
	switch (gate_status) {
	case OPEN:
		return "CHARGED";
	case CLOSE:
		return "DROPPED";
	}
	return "ERROR IN ADC RULE";
}

static const char *
sponsor_id_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	if (pcc_rule)
		return pcc_rule->sponsor_id;
	else if (adc_rule)
		return adc_rule->sponsor_id;
	PANIC_ON_UNDEFINED_RULE();
	return 0;
}

static uint32_t
service_id_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	if (pcc_rule)
		return pcc_rule->service_id;
	else if (adc_rule)
		return adc_rule->service_id;
	PANIC_ON_UNDEFINED_RULE();
	return 0;
}

static uint32_t
rate_group_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	if (pcc_rule)
		return pcc_rule->rating_group;
	else if (adc_rule)
		return adc_rule->rating_group;
	PANIC_ON_UNDEFINED_RULE();
	return 0;
}

static const char *
tarriff_group_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	if (pcc_rule)
		return "(null)";
	else if (adc_rule)
		return adc_rule->tarriff_group;
	PANIC_ON_UNDEFINED_RULE();
	return NULL;
}

static const char *
tarriff_time_cb(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule) {
	if (pcc_rule)
		return "(null)";
	else if (adc_rule)
		return adc_rule->tarriff_time;
	PANIC_ON_UNDEFINED_RULE();
	return NULL;
}

/* cdr field definition macros */
#define DEFINE_VALUE(head, val) {\
	.header = head, \
	.type = CDR_VALUE, \
	.format_specifier = "%"PRIu64",", \
	.value = val}
#define DEFINE_CB_64(head, func) {\
	.header = head, \
	.type = CDR_CB_64, \
	.format_specifier = "%"PRIu64",", \
	.cb_64 = func}
#define DEFINE_CB_32(head, func) {\
	.header = head, \
	.type = CDR_CB_32, \
	.format_specifier = "%"PRIu64",", \
	.cb_32 = func}
#define DEFINE_CB_STR(head, func) {\
	.header = head, \
	.type = CDR_CB_STR, \
	.format_specifier = "%s,", \
	.cb_str = func}

/* define cdr fields */
struct cdr_field_t cdr_fields[] = {
		DEFINE_VALUE("record", &cdr_count),
		DEFINE_CB_STR("time", cdr_time_cb),
		DEFINE_CB_STR("ue_ip", ue_ip_cb),
		DEFINE_CB_64("dl_pkt_cnt", dl_pkt_cnt_cb),
		DEFINE_CB_64("dl_bytes", dl_byptes_cb),
		DEFINE_CB_64("ul_pkt_cnt", ul_pkt_cnt_cb),
		DEFINE_CB_64("ul_bytes", ul_byptes_cb),
		DEFINE_CB_32("rule_id", rule_id_cb),
		DEFINE_CB_STR("rule_type", rule_type_cb),
		DEFINE_CB_STR("rule", rule_cb),
		DEFINE_CB_STR("action", action_cb),
		DEFINE_CB_STR("sponsor_id", sponsor_id_cb),
		DEFINE_CB_32("service_id", service_id_cb),
		DEFINE_CB_32("rate_group", rate_group_cb),
		DEFINE_CB_STR("tarriff_group", tarriff_group_cb),
		DEFINE_CB_STR("tarriff_time", tarriff_time_cb),
};

static void
write_to_cdr(void)
{
	fflush(cdr_file_stream);
	fwrite(cdr_file_stream_ptr, sizeof(char),
			cdr_file_stream_sizeloc, cdr_file);
	rewind(cdr_file_stream);
}


/**
 * Writes the CDR field headers to file
 */
static void
export_cdr_field_headers(void)
{
	unsigned i;
	fprintf(cdr_file_stream, "#");
	for (i = 0; i < RTE_DIM(cdr_fields); ++i)
		fprintf(cdr_file_stream, "%s,", cdr_fields[i].header);
	fprintf(cdr_file_stream, "\n");

	write_to_cdr();
}

/**
 * common function to export pcc & adc cdr
 * @param session
 *	session to charge
 * @param vol
 *	cdr volume
 * @param pcc_rule
 *	pcc_rule - must be NULL if CDR applies to ADC rule
 * @param adc_rule
 *	adc rule - nust be NULL if CDR applies to PCC rule
 *
 * NOTE: common function for both ADC and PCC rules. Either adc_rule OR pcc_rule
 * must be defined - not both.
 */
static void
export_record(struct dp_session_info *session,
		struct chrg_data_vol *vol,
		struct dp_pcc_rules *pcc_rule,
		struct adc_rules *adc_rule)
{
	unsigned i;
	if (pcc_rule != NULL && adc_rule != NULL)
		PANIC_ON_UNDEFINED_RULE();

	if (!session)
		return;

	if (!(vol->dl_cdr.pkt_count || vol->ul_cdr.pkt_count
			|| vol->dl_drop.pkt_count || vol->ul_drop.pkt_count))
		return;

	for (i = 0; i < RTE_DIM(cdr_fields); ++i) {
		switch (cdr_fields[i].type) {
		case CDR_VALUE:
			fprintf(cdr_file_stream, cdr_fields[i].format_specifier,
					*cdr_fields[i].value);
			break;
		case CDR_CB_STR:
			fprintf(cdr_file_stream, cdr_fields[i].format_specifier,
					cdr_fields[i].cb_str(session,
							vol, pcc_rule,
							adc_rule));
			break;
		case CDR_CB_32:
			fprintf(cdr_file_stream, cdr_fields[i].format_specifier,
					cdr_fields[i].cb_32(session,
							vol, pcc_rule,
							adc_rule));
			break;
		case CDR_CB_64:
			fprintf(cdr_file_stream, cdr_fields[i].format_specifier,
					cdr_fields[i].cb_64(session,
							vol, pcc_rule,
							adc_rule));
			break;
		}
	}
	++cdr_count;
	fprintf(cdr_file_stream, "\n");

	write_to_cdr();
}

void
set_cdr_path(const char *path)
{
	size_t append_end_slash = path[strlen(path) - 1] == '/' ? 0 : 1;
	size_t alloc_size = strlen(path) + 1 + append_end_slash;
	if (alloc_size > PATH_MAX)
		rte_panic("cdr_path argument exceeds system max "
				"length of %u: %s", PATH_MAX, path);
	cdr_path = malloc(alloc_size);
	strcpy(cdr_path, path);
	if (append_end_slash)
		strcat(cdr_path, "/");
}

static void
create_sys_path(char *path)
{
	DIR *dir = opendir(path);
	char *parent = strrchr(path, '/');
	int ret;
	if (dir) {
		closedir(dir);
		return;
	}
	if (errno != ENOENT)
		rte_panic("cdr_path error: %s\n", strerror(errno));
	if (parent == NULL)
		rte_panic("Cannot parse cdr_path parent directory: %s\n", path);
	*parent = '\0';
	create_sys_path(path);
	*parent = '/';
	ret = mkdir(path, S_IRWXU);
	if (ret && errno != EEXIST)
		rte_panic("Failed to create directory %s: %s\n", path,
				strerror(errno));
}

static void
create_new_cdr_file(void)
{
	char timestamp[NAME_MAX];
	char filename[PATH_MAX];
	int ret;
	time_t t = time(NULL);
	struct tm *tmp = localtime(&t);

	if (tmp == NULL)
		rte_panic("Failed to obtain CDR timestamp\n");

	ret = strftime(timestamp, NAME_MAX, "%Y%m%d%H%M%S", tmp);
	if (ret == 0)
		rte_panic("Failed to generate CDR timestamp\n");

#ifdef SDN_ODL_BUILD
	ret = snprintf(filename, PATH_MAX, "%s%s_%s"CDR_CSV_EXTENSION, cdr_path,
			node_id, timestamp);
#else
	ret = snprintf(filename, PATH_MAX, "%s%s"CDR_CSV_EXTENSION, cdr_path,
			timestamp);
#endif

	if (ret < 0)
		rte_panic("output error during cdr filename creation\n");
	if (ret > PATH_MAX)
		rte_panic("cdr filename and path exceeds system limits\n");

	printf("Logging CDR Records to %s\n", filename);

	cdr_count = 0;
	cdr_file = fopen(filename, "w");
	if (!cdr_file)
		rte_panic("CDR file %s failed to open for writing\n - %s (%d)",
					filename, strerror(errno), errno);

	setvbuf(cdr_file, NULL, _IOLBF, BUFFER_SIZE);

	cdr_file_stream = open_memstream(&cdr_file_stream_ptr,
			&cdr_file_stream_sizeloc);
	if (!cdr_file_stream)
		rte_panic("CDR file stream failed to open\n - %s (%d)",
					strerror(errno), errno);

	export_cdr_field_headers();
}

void
cdr_init(void)
{
	create_sys_path(cdr_path);

	create_new_cdr_file();
}

void
export_session_pcc_record(struct dp_pcc_rules *pcc_rule,
				struct ipcan_dp_bearer_cdr *charge_record,
				struct dp_session_info *session)
{
	export_record(session, &charge_record->data_vol, pcc_rule, NULL);
}

void
export_session_adc_record(struct adc_rules *adc_rule,
				struct ipcan_dp_bearer_cdr *charge_record,
				struct dp_session_info *session)
{
	export_record(session, &charge_record->data_vol, NULL, adc_rule);
}
