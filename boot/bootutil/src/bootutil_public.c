/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2017-2019 Linaro LTD
 * Copyright (c) 2016-2019 JUUL Labs
 * Copyright (c) 2019-2021 Arm Limited
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * Original license:
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * @file
 * @brief Public MCUBoot interface API implementation
 *
 * This file contains API implementation which can be combined with
 * the application in order to interact with the MCUBoot bootloader.
 * This file contains shared code-base betwen MCUBoot and the application
 * which controls DFU process.
 */

#include <string.h>
#include <inttypes.h>
#include <stddef.h>

#include "sysflash/sysflash.h"
#include "flash_map_backend/flash_map_backend.h"

#include "bootutil/image.h"
#include "bootutil/bootutil_public.h"
#include "bootutil/bootutil_log.h"
#include "swap_status.h"
#ifdef MCUBOOT_ENC_IMAGES
#include "bootutil/enc_key_public.h"
#endif

#include "bootutil/boot_public_hooks.h"

#ifdef CONFIG_MCUBOOT
BOOT_LOG_MODULE_DECLARE(mcuboot);
#else
BOOT_LOG_MODULE_REGISTER(mcuboot_util);
#endif

const uint32_t boot_img_magic[] = {
    0xf395c277,
    0x7fefd260,
    0x0f505235,
    0x8079b62c,
};

#define BOOT_MAGIC_ARR_SZ \
    (sizeof boot_img_magic / sizeof boot_img_magic[0])

struct boot_swap_table {
    uint8_t magic_primary_slot;
    uint8_t magic_secondary_slot;
    uint8_t image_ok_primary_slot;
    uint8_t image_ok_secondary_slot;
    uint8_t copy_done_primary_slot;

    uint8_t swap_type;
};

/**
 * This set of tables maps image trailer contents to swap operation type.
 * When searching for a match, these tables must be iterated sequentially.
 *
 * NOTE: the table order is very important. The settings in the secondary
 * slot always are priority to the primary slot and should be located
 * earlier in the table.
 *
 * The table lists only states where there is action needs to be taken by
 * the bootloader, as in starting/finishing a swap operation.
 */
static const struct boot_swap_table boot_swap_tables[] = {
    {
        .magic_primary_slot =       BOOT_MAGIC_ANY,
        .magic_secondary_slot =     BOOT_MAGIC_GOOD,
        .image_ok_primary_slot =    BOOT_FLAG_ANY,
        .image_ok_secondary_slot =  BOOT_FLAG_UNSET,
        .copy_done_primary_slot =   BOOT_FLAG_ANY,
        .swap_type =                BOOT_SWAP_TYPE_TEST,
    },
    {
        .magic_primary_slot =       BOOT_MAGIC_ANY,
        .magic_secondary_slot =     BOOT_MAGIC_GOOD,
        .image_ok_primary_slot =    BOOT_FLAG_ANY,
        .image_ok_secondary_slot =  BOOT_FLAG_SET,
        .copy_done_primary_slot =   BOOT_FLAG_ANY,
        .swap_type =                BOOT_SWAP_TYPE_PERM,
    },
    {
        .magic_primary_slot =       BOOT_MAGIC_GOOD,
        .magic_secondary_slot =     BOOT_MAGIC_UNSET,
        .image_ok_primary_slot =    BOOT_FLAG_UNSET,
        .image_ok_secondary_slot =  BOOT_FLAG_ANY,
        .copy_done_primary_slot =   BOOT_FLAG_SET,
        .swap_type =                BOOT_SWAP_TYPE_REVERT,
    },
};

#define BOOT_SWAP_TABLES_COUNT \
    (sizeof boot_swap_tables / sizeof boot_swap_tables[0])

#ifndef MCUBOOT_SWAP_USING_STATUS
static int
boot_magic_decode(const uint32_t *magic)
{
    if (memcmp(magic, boot_img_magic, BOOT_MAGIC_SZ) == 0) {
        return BOOT_MAGIC_GOOD;
    }
    return BOOT_MAGIC_BAD;
}
#endif /* !MCUBOOT_SWAP_USING_STATUS */

static int
boot_flag_decode(uint8_t flag)
{
    if (flag != BOOT_FLAG_SET) {
        return BOOT_FLAG_BAD;
    }
    return BOOT_FLAG_SET;
}

#ifndef MCUBOOT_SWAP_USING_STATUS
static inline uint32_t
boot_magic_off(const struct flash_area *fap)
{
    return flash_area_get_size(fap) - BOOT_MAGIC_SZ;
}

static inline uint32_t
boot_image_ok_off(const struct flash_area *fap)
{
    return boot_magic_off(fap) - BOOT_MAX_ALIGN;
}

static inline uint32_t
boot_copy_done_off(const struct flash_area *fap)
{
    return boot_image_ok_off(fap) - BOOT_MAX_ALIGN;
}

uint32_t
boot_swap_info_off(const struct flash_area *fap)
{
    return boot_copy_done_off(fap) - BOOT_MAX_ALIGN;
}
#endif /* !MCUBOOT_SWAP_USING_STATUS */

/**
 * Determines if a status source table is satisfied by the specified magic
 * code.
 *
 * @param tbl_val               A magic field from a status source table.
 * @param val                   The magic value in a trailer, encoded as a
 *                                  BOOT_MAGIC_[...].
 *
 * @return                      true - if the two values are compatible;
 *                              false - otherwise.
 */
bool boot_magic_compatible_check(uint8_t tbl_val, uint8_t val)
{
    switch (tbl_val) {
    case BOOT_MAGIC_ANY:
        return true;

    case BOOT_MAGIC_NOTGOOD:
        return val != BOOT_MAGIC_GOOD;

    default:
        return tbl_val == val;
    }
}

bool bootutil_buffer_is_filled(const void *buffer, uint8_t fill, size_t len)
{
    uint8_t *p;

    if (buffer == NULL || len == 0) {
        return false;
    }

    for (p = (uint8_t *)buffer; len-- > 0; p++) {
        if (*p != fill) {
            return false;
        }
    }

    return true;
}

bool bootutil_buffer_is_erased(const struct flash_area *area,
                               const void *buffer, size_t len)
{
    uint8_t erased_val;

    if (area == NULL) {
        return false;
    }

    erased_val = flash_area_erased_val(area);

    return bootutil_buffer_is_filled(buffer, erased_val, len);
}

static int
boot_read_flag(const struct flash_area *fap, uint8_t *flag, uint32_t off)
{
    int rc;

    rc = flash_area_read(fap, off, flag, sizeof *flag);
    if (rc != 0) {
        return BOOT_EFLASH;
    }
    if (*flag == flash_area_erased_val(fap)) {
        *flag = BOOT_FLAG_UNSET;
    } else {
        *flag = boot_flag_decode(*flag);
    }

    return 0;
}

#ifndef MCUBOOT_SWAP_USING_STATUS

static inline int
boot_read_copy_done(const struct flash_area *fap, uint8_t *copy_done)
{
    return boot_read_flag(fap, copy_done, boot_copy_done_off(fap));
}


int
boot_read_swap_state(const struct flash_area *fap,
                     struct boot_swap_state *state)
{
    uint32_t magic[BOOT_MAGIC_ARR_SZ];
    uint32_t off;
    uint8_t swap_info;
    int rc;

    off = boot_magic_off(fap);
    rc = flash_area_read(fap, off, magic, BOOT_MAGIC_SZ);
    if (rc != 0) {
        return BOOT_EFLASH;
    }
    if (bootutil_buffer_is_erased(fap, magic, BOOT_MAGIC_SZ)) {
        state->magic = BOOT_MAGIC_UNSET;
    } else {
        state->magic = boot_magic_decode(magic);
    }

    off = boot_swap_info_off(fap);
    rc = flash_area_read(fap, off, &swap_info, sizeof swap_info);
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    /* Extract the swap type and image number */
    state->swap_type = BOOT_GET_SWAP_TYPE(swap_info);
    state->image_num = BOOT_GET_IMAGE_NUM(swap_info);

    if (swap_info == flash_area_erased_val(fap) ||
            state->swap_type > BOOT_SWAP_TYPE_REVERT) {
        state->swap_type = BOOT_SWAP_TYPE_NONE;
        state->image_num = 0;
    }

    rc = boot_read_copy_done(fap, &state->copy_done);
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    return boot_read_image_ok(fap, &state->image_ok);
}

#endif /* !MCUBOOT_SWAP_USING_STATUS */

int
boot_read_swap_state_by_id(int flash_area_id, struct boot_swap_state *state)
{
    const struct flash_area *fap = NULL;
    int rc;

    rc = flash_area_open(flash_area_id, &fap);
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    rc = boot_read_swap_state(fap, state);
    flash_area_close(fap);
    return rc;
}

#ifndef MCUBOOT_SWAP_USING_STATUS

int
boot_write_magic(const struct flash_area *fap)
{
    uint32_t off;
    int rc;

    off = boot_magic_off(fap);

    BOOT_LOG_DBG("writing magic; fa_id=%u off=0x%" PRIx32
                 " (0x%" PRIx32 ")", (unsigned)flash_area_get_id(fap),
                 off, flash_area_get_off(fap) + off);
    rc = flash_area_write(fap, off, boot_img_magic, BOOT_MAGIC_SZ);
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    return 0;
}

/**
 * Write trailer data; status bytes, swap_size, etc
 *
 * @returns 0 on success, != 0 on error.
 */
int
boot_write_trailer(const struct flash_area *fap, uint32_t off,
        const uint8_t *inbuf, uint8_t inlen)
{
    uint8_t buf[BOOT_MAX_ALIGN];
    size_t align;
    uint8_t erased_val;
    int rc;

    align = flash_area_align(fap);
    if (align == 0u) {
        return BOOT_EFLASH;
    }
    align = (inlen + align - 1) & ~(align - 1);
    if (align > BOOT_MAX_ALIGN) {
        return -1;
    }
    erased_val = flash_area_erased_val(fap);

    memcpy(buf, inbuf, inlen);
    memset(&buf[inlen], erased_val, align - inlen);

    rc = flash_area_write(fap, off, buf, align);
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    return 0;
}

#endif /* !MCUBOOT_SWAP_USING_STATUS */

int
boot_write_trailer_flag(const struct flash_area *fap, uint32_t off,
        uint8_t flag_val)
{
    const uint8_t buf[1] = { flag_val };
    return boot_write_trailer(fap, off, buf, 1);
}

int
boot_write_image_ok(const struct flash_area *fap)
{
    uint32_t off;

    off = boot_image_ok_off(fap);
    BOOT_LOG_DBG("writing image_ok; fa_id=%u off=0x%" PRIx32
                 " (0x%" PRIx32 ")", (unsigned)flash_area_get_id(fap),
                 off, flash_area_get_off(fap) + off);
    return boot_write_trailer_flag(fap, off, BOOT_FLAG_SET);
}

int
boot_read_image_ok(const struct flash_area *fap, uint8_t *image_ok)
{
    return boot_read_flag(fap, image_ok, boot_image_ok_off(fap));
}

/**
 * Writes the specified value to the `swap-type` field of an image trailer.
 * This value is persisted so that the boot loader knows what swap operation to
 * resume in case of an unexpected reset.
 */
int
boot_write_swap_info(const struct flash_area *fap, uint8_t swap_type,
                     uint8_t image_num)
{
    uint32_t off;
    uint8_t swap_info;

    BOOT_SET_SWAP_INFO(swap_info, image_num, swap_type);
    off = boot_swap_info_off(fap);
    BOOT_LOG_DBG("writing swap_info; fa_id=%u off=0x%" PRIx32
                 " (0x%" PRIx32 "), swap_type=0x%x image_num=0x%x",
                 (unsigned)flash_area_get_id(fap), off,
                 flash_area_get_off(fap) + off,
                 (unsigned)swap_type, (unsigned)image_num);
    return boot_write_trailer(fap, off, (const uint8_t *) &swap_info, 1);
}

#define BOOT_LOG_SWAP_STATE(area, state)                                        \
    BOOT_LOG_INF("%s: magic=%s, swap_type=0x%x, copy_done=0x%x, image_ok=0x%x", \
                 (area),                                                        \
                 ((state)->magic == BOOT_MAGIC_GOOD  ? "good" :                 \
                  (state)->magic == BOOT_MAGIC_UNSET ? "unset" :                \
                                                       "bad"),                  \
                 (unsigned)(state)->swap_type,                                  \
                 (unsigned)(state)->copy_done,                                  \
                 (unsigned)(state)->image_ok)

int
boot_swap_type_multi(int image_index)
{
    const struct boot_swap_table *table = NULL;
    struct boot_swap_state primary_slot = {0};
    struct boot_swap_state secondary_slot = {0};
    int rc;
    size_t i;

    rc = BOOT_HOOK_CALL(boot_read_swap_state_primary_slot_hook,
                        BOOT_HOOK_REGULAR, image_index, &primary_slot);
    if (rc == BOOT_HOOK_REGULAR)
    {
        rc = boot_read_swap_state_by_id(FLASH_AREA_IMAGE_PRIMARY(image_index),
                                        &primary_slot);
    }
    if (rc) {
        return BOOT_SWAP_TYPE_PANIC;
    }

    rc = boot_read_swap_state_by_id(FLASH_AREA_IMAGE_SECONDARY(image_index),
                                    &secondary_slot);
    if (rc == BOOT_EFLASH) {
        BOOT_LOG_INF("Secondary image of image pair (%d.) "
                     "is unreachable. Treat it as empty", image_index);
        secondary_slot.magic = BOOT_MAGIC_UNSET;
        secondary_slot.swap_type = BOOT_SWAP_TYPE_NONE;
        secondary_slot.copy_done = BOOT_FLAG_UNSET;
        secondary_slot.image_ok = BOOT_FLAG_UNSET;
        secondary_slot.image_num = 0;
    } else if (rc) {
        return BOOT_SWAP_TYPE_PANIC;
    }

    BOOT_LOG_SWAP_STATE("boot_swap_type_multi: Primary image", &primary_slot);
    BOOT_LOG_SWAP_STATE("boot_swap_type_multi: Secondary image", &secondary_slot);

    for (i = 0; i < BOOT_SWAP_TABLES_COUNT; i++) {
        table = boot_swap_tables + i;

        if (boot_magic_compatible_check(table->magic_primary_slot,
                                        primary_slot.magic) &&
            boot_magic_compatible_check(table->magic_secondary_slot,
                                        secondary_slot.magic) &&
            (table->image_ok_primary_slot == BOOT_FLAG_ANY   ||
                table->image_ok_primary_slot == primary_slot.image_ok) &&
            (table->image_ok_secondary_slot == BOOT_FLAG_ANY ||
                table->image_ok_secondary_slot == secondary_slot.image_ok) &&
            (table->copy_done_primary_slot == BOOT_FLAG_ANY  ||
                table->copy_done_primary_slot == primary_slot.copy_done)) {
            BOOT_LOG_INF("Swap type: %s",
                         table->swap_type == BOOT_SWAP_TYPE_TEST   ? "test"   :
                         table->swap_type == BOOT_SWAP_TYPE_PERM   ? "perm"   :
                         table->swap_type == BOOT_SWAP_TYPE_REVERT ? "revert" :
                         "BUG; can't happen");
            if (table->swap_type != BOOT_SWAP_TYPE_TEST &&
                    table->swap_type != BOOT_SWAP_TYPE_PERM &&
                    table->swap_type != BOOT_SWAP_TYPE_REVERT) {
                return BOOT_SWAP_TYPE_PANIC;
            }
            return table->swap_type;
        }
    }

    BOOT_LOG_INF("Swap type: none");
    return BOOT_SWAP_TYPE_NONE;
}

/*
 * This function is not used by the bootloader itself, but its required API
 * by external tooling like mcumgr.
 */
int
boot_swap_type(void)
{
    return boot_swap_type_multi(0);
}

/**
 * Marks the image with the given index in the secondary slot as pending. On the
 * next reboot, the system will perform a one-time boot of the the secondary
 * slot image.
 *
 * @param image_index       Image pair index.
 *
 * @param permanent         Whether the image should be used permanently or
 *                          only tested once:
 *                               0=run image once, then confirm or revert.
 *                               1=run image forever.
 *
 * @return                  0 on success; nonzero on failure.
 */
int
boot_set_pending_multi(int image_index, int permanent)
{
    const struct flash_area *fap = NULL;
    struct boot_swap_state state_secondary_slot = {0};
    uint8_t swap_type;
    int rc;

    rc = flash_area_open(FLASH_AREA_IMAGE_SECONDARY(image_index), &fap);
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    rc = boot_read_swap_state(fap, &state_secondary_slot);
    if (rc != 0) {
        goto done;
    }

    switch (state_secondary_slot.magic) {
    case BOOT_MAGIC_GOOD:
        /* Swap already scheduled. */
        break;

    case BOOT_MAGIC_UNSET:
        rc = boot_write_magic(fap);

        if (rc == 0 && permanent) {
            rc = boot_write_image_ok(fap);
        }

        if (rc == 0) {
            if (permanent) {
                swap_type = BOOT_SWAP_TYPE_PERM;
            } else {
                swap_type = BOOT_SWAP_TYPE_TEST;
            }
            rc = boot_write_swap_info(fap, swap_type, 0);
        }

        break;

    case BOOT_MAGIC_BAD:
        /* The image slot is corrupt.  There is no way to recover, so erase the
         * slot to allow future upgrades.
         */
        flash_area_erase(fap, 0, flash_area_get_size(fap));
        rc = BOOT_EBADIMAGE;
        break;

    default:
        assert(0);
        rc = BOOT_EBADIMAGE;
    }

done:
    flash_area_close(fap);
    return rc;
}

/**
 * Marks the image with index 0 in the secondary slot as pending. On the next
 * reboot, the system will perform a one-time boot of the the secondary slot
 * image. Note that this API is kept for compatibility. The
 * boot_set_pending_multi() API is recommended.
 *
 * @param permanent         Whether the image should be used permanently or
 *                          only tested once:
 *                               0=run image once, then confirm or revert.
 *                               1=run image forever.
 *
 * @return                  0 on success; nonzero on failure.
 */
int
boot_set_pending(int permanent)
{
    return boot_set_pending_multi(0, permanent);
}

/**
 * Marks the image with the given index in the primary slot as confirmed.  The
 * system will continue booting into the image in the primary slot until told to
 * boot from a different slot.
 *
 * @param image_index       Image pair index.
 *
 * @return                  0 on success; nonzero on failure.
 */
int
boot_set_confirmed_multi(int image_index)
{
    const struct flash_area *fap = NULL;
    struct boot_swap_state state_primary_slot = {0};
    int rc;

    rc = flash_area_open(FLASH_AREA_IMAGE_PRIMARY(image_index), &fap);
    if (rc != 0) {
        return BOOT_EFLASH;
    }

    rc = boot_read_swap_state(fap, &state_primary_slot);
    if (rc != 0) {
        goto done;
    }

    switch (state_primary_slot.magic) {
    case BOOT_MAGIC_GOOD:
        /* Confirm needed; proceed. */
        break;

    case BOOT_MAGIC_UNSET:
        /* Already confirmed. */
        goto done;

    case BOOT_MAGIC_BAD:
        /* Unexpected state. */
        rc = BOOT_EBADVECT;
        goto done;
    }

    /* Intentionally do not check copy_done flag
     * so can confirm a padded image which was programed using a programing
     * interface.
     */

    if (state_primary_slot.image_ok != BOOT_FLAG_UNSET) {
        /* Already confirmed. */
        goto done;
    }

    rc = boot_write_image_ok(fap);

done:
    flash_area_close(fap);
    return rc;
}

/**
 * Marks the image with index 0 in the primary slot as confirmed.  The system
 * will continue booting into the image in the primary slot until told to boot
 * from a different slot.  Note that this API is kept for compatibility. The
 * boot_set_confirmed_multi() API is recommended.
 *
 * @return                  0 on success; nonzero on failure.
 */
int
boot_set_confirmed(void)
{
    return boot_set_confirmed_multi(0);
}
