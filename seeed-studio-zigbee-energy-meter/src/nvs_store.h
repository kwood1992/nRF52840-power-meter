#ifndef NVS_STORE_H
#define NVS_STORE_H

#include <stdint.h>

/*
 * Thin wrapper around Zephyr NVS on the DT `storage_partition` (0x000ec000,
 * 32 KB on the xiao_ble). Persists a single uint64_t: the pulse
 * accumulator's cumulative total.
 *
 * Not host-testable — depends on Zephyr's flash + NVS subsystems. The
 * *policy* of when to write is factored out into persist_policy.[ch]
 * which is host-tested.
 */

int nvs_store_init(void);

/*
 * On hit, writes the saved total into *out and returns 0.
 * On cold boot (no entry yet), returns -ENOENT and leaves *out untouched.
 * Any other error returned as-is from the NVS layer.
 */
int nvs_store_load_total(uint64_t *out);

int nvs_store_save_total(uint64_t total);

#endif
