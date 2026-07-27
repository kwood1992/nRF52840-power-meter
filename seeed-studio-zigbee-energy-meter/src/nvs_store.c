#include "nvs_store.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(nvs_store, LOG_LEVEL_INF);

/* One ID per persisted value. IDs are stable schema — never renumber,
 * never re-purpose. On the wear-leveled NVS partition the ID is what
 * matches a record to its slot, so a rename would silently orphan the
 * old data.
 */
#define NVS_ID_ACCUMULATOR_TOTAL 1U
#define NVS_ID_IMP_PER_KWH       2U  /* Metering Divisor override, issue #48 */

static struct nvs_fs fs;
static bool initialized;

int nvs_store_init(void)
{
	if (initialized) {
		return 0;
	}

	fs.flash_device = FIXED_PARTITION_DEVICE(nvs_storage);
	if (!device_is_ready(fs.flash_device)) {
		return -ENODEV;
	}

	fs.offset = FIXED_PARTITION_OFFSET(nvs_storage);

	struct flash_pages_info info;
	int rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);

	if (rc) {
		return rc;
	}

	fs.sector_size = info.size;
	/* nvs_storage is 32 KB = 8 x 4 KB sectors on nRF52840. Give
	 * NVS all of them so wear-leveling has maximum runway.
	 */
	fs.sector_count = FIXED_PARTITION_SIZE(nvs_storage) / info.size;

	rc = nvs_mount(&fs);
	if (rc) {
		/* Field-realistic failure: partition contents inconsistent with
		 * what NVS expects (prior firmware layout, partial-write from a
		 * cut power event, bit rot in sector headers). Wipe the
		 * partition and retry once — losing the accumulator value is
		 * strictly better than perpetual data loss until reflash.
		 * See issue #15.
		 */
		int clr = nvs_clear(&fs);

		if (clr == 0) {
			rc = nvs_mount(&fs);
		}
		if (rc) {
			return rc;
		}
		LOG_WRN("recovered NVS after mount failure — accumulator reset to 0");
	}

	initialized = true;
	return 0;
}

int nvs_store_load_total(uint64_t *out)
{
	if (!initialized) {
		return -EINVAL;
	}

	uint64_t val = 0;
	ssize_t n = nvs_read(&fs, NVS_ID_ACCUMULATOR_TOTAL, &val, sizeof(val));

	if (n == sizeof(val)) {
		*out = val;
		return 0;
	}
	if (n < 0) {
		return (int)n;
	}
	/* Short read = never written or corrupted — treat as cold boot. */
	return -ENOENT;
}

int nvs_store_save_total(uint64_t total)
{
	if (!initialized) {
		return -EINVAL;
	}

	ssize_t n = nvs_write(&fs, NVS_ID_ACCUMULATOR_TOTAL, &total, sizeof(total));

	if (n < 0) {
		return (int)n;
	}
	return 0;
}

int nvs_store_load_imp_per_kwh(uint32_t *out)
{
	if (!initialized) {
		return -EINVAL;
	}

	uint32_t val = 0;
	ssize_t n = nvs_read(&fs, NVS_ID_IMP_PER_KWH, &val, sizeof(val));

	if (n == sizeof(val)) {
		*out = val;
		return 0;
	}
	if (n < 0) {
		return (int)n;
	}
	/* Never written or short read — treat as "use compile-time default". */
	return -ENOENT;
}

int nvs_store_save_imp_per_kwh(uint32_t imp_per_kwh)
{
	if (!initialized) {
		return -EINVAL;
	}

	ssize_t n = nvs_write(&fs, NVS_ID_IMP_PER_KWH,
			      &imp_per_kwh, sizeof(imp_per_kwh));

	if (n < 0) {
		return (int)n;
	}
	return 0;
}
