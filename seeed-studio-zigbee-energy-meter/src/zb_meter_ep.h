#ifndef ZB_METER_EP_H
#define ZB_METER_EP_H 1

/*
 * Custom Zigbee endpoint declaration for the pulse-counting power-meter
 * app. Publishes three server clusters:
 *
 *   * Basic     (0x0000) — zcl_version + power_source
 *   * Identify  (0x0003) — identify_time (Z2M's pair-time blink hook)
 *   * Metering  (0x0702) — CurrentSummationDelivered as a live u48 of
 *                          the pulse accumulator, plus Multiplier /
 *                          Divisor so Z2M reads kWh natively (see the
 *                          design doc's Zigbee-model row).
 *
 * Pattern is a stripped-down copy of ncs-zigbee's
 * `ZB_HA_DECLARE_SMART_PLUG_*` macros — reporting_ctx sized for the
 * mandatory metering-report attributes, no CVC ctx. Not derived from
 * the temporary `zb_range_extender.h` skeleton that lived here in #4;
 * that device type was only there so ZBOSS had *something* to
 * advertise during commissioning, and #5 replaces it wholesale.
 *
 * Assumes zboss headers (zb_zcl_metering.h, zboss_api_af.h) are
 * already on the include path — include this only from a translation
 * unit that has already pulled in <zboss_api.h>.
 */

/* Custom-app "Metering Device" HA device ID. Zigbee HA doesn't define a
 * canonical metering device ID; Z2M identifies devices by cluster
 * fingerprint + manufacturer/model rather than looking this up in any
 * table, so the number here is only informative. Reused across all
 * builds of this project so external converters can pin it.
 */
#define ZB_METER_DEVICE_ID   0x0053
#define ZB_METER_DEVICE_VER  0

/* Server clusters: Basic, Identify, Metering. */
#define ZB_METER_IN_CLUSTER_NUM   3

/* No client clusters (nothing on the app endpoint needs to send
 * commands to a peer).
 */
#define ZB_METER_OUT_CLUSTER_NUM  0

/* Reporting-info slots reserved for this endpoint. The metering
 * cluster header declares `ZB_ZCL_METERING_REPORT_ATTR_COUNT = 3`
 * as the mandatory-reportable count — enough headroom for
 * CurrentSummationDelivered plus two more attributes if we ever add
 * them. Storage is a few tens of bytes per slot; overbudgeting
 * slightly is cheap.
 */
#define ZB_METER_REPORT_ATTR_COUNT  ZB_ZCL_METERING_REPORT_ATTR_COUNT


#define ZB_DECLARE_METER_CLUSTER_LIST(                                    \
		cluster_list_name,                                        \
		basic_attr_list,                                          \
		identify_attr_list,                                       \
		metering_attr_list)                                       \
zb_zcl_cluster_desc_t cluster_list_name[] =                               \
{                                                                         \
	ZB_ZCL_CLUSTER_DESC(                                              \
		ZB_ZCL_CLUSTER_ID_IDENTIFY,                               \
		ZB_ZCL_ARRAY_SIZE(identify_attr_list, zb_zcl_attr_t),     \
		(identify_attr_list),                                     \
		ZB_ZCL_CLUSTER_SERVER_ROLE,                               \
		ZB_ZCL_MANUF_CODE_INVALID                                 \
	),                                                                \
	ZB_ZCL_CLUSTER_DESC(                                              \
		ZB_ZCL_CLUSTER_ID_BASIC,                                  \
		ZB_ZCL_ARRAY_SIZE(basic_attr_list, zb_zcl_attr_t),        \
		(basic_attr_list),                                        \
		ZB_ZCL_CLUSTER_SERVER_ROLE,                               \
		ZB_ZCL_MANUF_CODE_INVALID                                 \
	),                                                                \
	ZB_ZCL_CLUSTER_DESC(                                              \
		ZB_ZCL_CLUSTER_ID_METERING,                               \
		ZB_ZCL_ARRAY_SIZE(metering_attr_list, zb_zcl_attr_t),     \
		(metering_attr_list),                                     \
		ZB_ZCL_CLUSTER_SERVER_ROLE,                               \
		ZB_ZCL_MANUF_CODE_INVALID                                 \
	)                                                                 \
}


#define ZB_ZCL_DECLARE_METER_SIMPLE_DESC(ep_name, ep_id, in_clust_num, out_clust_num) \
	ZB_DECLARE_SIMPLE_DESC(in_clust_num, out_clust_num);                          \
	ZB_AF_SIMPLE_DESC_TYPE(in_clust_num, out_clust_num) simple_desc_##ep_name =    \
	{                                                                              \
		ep_id,                                                                 \
		ZB_AF_HA_PROFILE_ID,                                                   \
		ZB_METER_DEVICE_ID,                                                    \
		ZB_METER_DEVICE_VER,                                                   \
		0,                                                                     \
		in_clust_num,                                                          \
		out_clust_num,                                                         \
		{                                                                      \
			ZB_ZCL_CLUSTER_ID_BASIC,                                       \
			ZB_ZCL_CLUSTER_ID_IDENTIFY,                                    \
			ZB_ZCL_CLUSTER_ID_METERING                                     \
		}                                                                      \
	}


#define ZB_DECLARE_METER_EP(ep_name, ep_id, cluster_list)                                  \
	ZB_ZCL_DECLARE_METER_SIMPLE_DESC(ep_name, ep_id,                                   \
		ZB_METER_IN_CLUSTER_NUM, ZB_METER_OUT_CLUSTER_NUM);                        \
	ZBOSS_DEVICE_DECLARE_REPORTING_CTX(reporting_info_##ep_name,                       \
		ZB_METER_REPORT_ATTR_COUNT);                                               \
	ZB_AF_DECLARE_ENDPOINT_DESC(ep_name, ep_id, ZB_AF_HA_PROFILE_ID, 0, NULL,          \
		ZB_ZCL_ARRAY_SIZE(cluster_list, zb_zcl_cluster_desc_t), cluster_list,      \
		(zb_af_simple_desc_1_1_t *)&simple_desc_##ep_name,                         \
		ZB_METER_REPORT_ATTR_COUNT, reporting_info_##ep_name,                      \
		0, NULL) /* No CVC ctx — Metering has no continuous-value attrs. */

#endif /* ZB_METER_EP_H */
