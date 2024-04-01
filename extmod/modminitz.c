/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Jon Foster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/smallint.h"

#define MICROPY_PY_MINITZ 1  // TODO

#if MICROPY_PY_MINITZ

#include "shared/timeutils/timeutils.h"

static const char mini_tzif_magic[8] = "EmTzif1\n";
static const char mini_tzdb_magic[8] = "EmTzDb1\n";
static const char mini_tzlist_magic[8] = "EmTzLi1\n";

// This is 9999-01-01 as a timestamp.
#define MINITZ_MAX_TIME 253370764800LL

// Max duration is 49708 days.  This is in seconds.
#define MINITZ_MAX_DURATION 4294771200U

// Min/max offset is a second under 26 hours.  This is in seconds.
#define MINITZ_MIN_OFFSET -93599
#define MINITZ_MAX_OFFSET 93599

#define MINITZ_MAGIC_CONSTANT 93600


typedef struct _mp_obj_minitz_db_t {
    mp_obj_base_t base;
    mp_obj_t data_obj;
    const byte *data_bytes;
    uint32_t data_len;
    int64_t start_time;
    uint32_t duration;
} mp_obj_minitz_db_t;

typedef struct _mp_obj_minitz_zone_t {
    mp_obj_base_t base;
    mp_obj_t db_obj;
    const byte *zone_start_bytes;
} mp_obj_minitz_zone_t;


static void minitz_parse_slice_like(mp_int_t size, mp_obj_t start_obj, mp_obj_t end_obj, mp_int_t *start_out, mp_int_t *end_out) {
    mp_int_t start = mp_obj_get_int(start_obj);
    mp_int_t end;
    if (end_obj == mp_const_none) {
        end = size;
    } else {
        end = mp_obj_get_int(end_obj);
        if (end <= 0) {
            end += size;
        }
        else if (end > size) {
            end = size;
        }
    }
    if (start < 0) {
        start += size;
    }
    if (start > end) {
        start = end;
    }
    *start_out = start;
    *end_out = end;
}

static void minitz_zone_get_subtimezone(mp_obj_minitz_zone_t *self, uint8_t stz_idx, mp_obj_t *out_array) {
    // Caller must ensure stz_idx is in range!
    const byte * stz_data = self->zone_start_bytes + 2 + stz_idx * 12u;

    const char * designator = stz_data;
    uint8_t is_dst = stz_data[7];
    int32_t offset = *(int32_t *)(stz_data + 8);

    out_array[0] = mp_obj_new_int(offset);
    out_array[1] = mp_obj_new_str_0(designator);
    out_array[2] = mp_obj_new_int(is_dst);
}


static int32_t minitz_zone_get_subtimezone_offset(mp_obj_minitz_zone_t *self, uint8_t stz_idx) {
    // Caller must ensure stz_idx is in range!
    const byte * stz_data = self->zone_start_bytes + 2 + stz_idx * 12u;

    return *(int32_t *)(stz_data + 8);
}

static uint32_t minitz_zone_get_subtimezone_adjusted_offset(mp_obj_minitz_zone_t *self, uint8_t stz_idx) {
    return (uint32_t)(MINITZ_MAGIC_CONSTANT + minitz_zone_get_subtimezone_offset(self, stz_idx));
}

static uint8_t minitz_zone_get_stz_and_turnback_duration(mp_obj_minitz_zone_t *self,
    const uint8_t * transition_stz_array,
    int32_t transition_index,
    uint32_t *turnback_duration) {

    uint8_t stz_idx = transition_stz_array[transition_index];

    // Calculate fold
    uint8_t prev_stz_idx = 0;
    if (transition_index != 0) {
        prev_stz_idx = transition_stz_array[transition_index - 1];
    }
    int32_t prev_offset = minitz_zone_get_subtimezone_offset(self, prev_stz_idx);
    int32_t cur_offset = minitz_zone_get_subtimezone_offset(self, stz_idx);
    int32_t turnback = prev_offset - cur_offset;
    if (turnback < 0) {
        turnback = 0;
    }
    *turnback_duration = (uint32_t)turnback;
    return stz_idx;
}

static NORETURN void raise_minitz_data_error(void) {
    mp_raise_ValueError(MP_ERROR_TEXT("timezone data invalid"));
}

// Zone.get_transitions(self, start: int, end: int) -> list[tuple[int, int, str, int]]
// Returns a list of: (when: int, offset: int, designator: str, is_dst: int)
//
// Returns a slice of the transitions array.
//
// Negative values of `start` or `end` mean to count from the end.
// A None value for `end` means to the end.
static mp_obj_t minitz_zone_get_transitions(mp_obj_t self_in, mp_obj_t start_obj, mp_obj_t end_obj) {
    mp_obj_minitz_zone_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t num_subtimezones = *(const uint8_t *)(self->zone_start_bytes);
    uint8_t num_transitions = *(const uint8_t *)(self->zone_start_bytes + 1);
    mp_int_t max_num_responses = (mp_int_t)num_transitions + 1;

    mp_int_t start;
    mp_int_t end;
    minitz_parse_slice_like(max_num_responses, start_obj, end_obj, &start, &end);

    const uint32_t * transition_when_array = (const uint32_t *)(self->zone_start_bytes + 2 + num_subtimezones * 12u);
    const uint8_t * transition_stz_array = (const uint8_t *)(transition_when_array + num_transitions);

    mp_obj_list_t *list = MP_OBJ_TO_PTR(mp_obj_new_list(end - start, NULL));

    uint32_t when = 0;
    uint8_t stz_idx = 0;
    for (mp_int_t ii = 0; start + ii < end; ++ii) {
        if (ii != 0 || start != 0) {
            when = transition_when_array[start + ii - 1];
            stz_idx = transition_stz_array[start + ii - 1];
        }

        mp_obj_minitz_db_t *db = MP_OBJ_TO_PTR(self->db_obj);

        mp_obj_t tuple[4];
        tuple[0] = mp_obj_new_int_from_ll(db->start_time + when);

        minitz_zone_get_subtimezone(self, stz_idx, tuple + 1);

        list->items[ii] = mp_obj_new_tuple(4, tuple);
    }

    return MP_OBJ_FROM_PTR(list);
}
static MP_DEFINE_CONST_FUN_OBJ_3(minitz_zone_get_transitions_obj, minitz_zone_get_transitions);

// Zone.lookup_utc(self, when: int) -> tuple[int, str, int, int]
// Returns: (offset: int, designator: str, is_dst: int, fold: int)
static mp_obj_t minitz_zone_lookup_utc(mp_obj_t self_in, mp_obj_t when_obj) {
    mp_obj_minitz_zone_t *self = MP_OBJ_TO_PTR(self_in);
    int64_t when = mp_obj_get_int_ll(when_obj);

    mp_obj_minitz_db_t *db = MP_OBJ_TO_PTR(self->db_obj);
    uint8_t num_transitions = *(const uint8_t *)(self->zone_start_bytes + 1);

    uint8_t stz_idx = 0;
    mp_int_t fold = 0;

    if (num_transitions != 0 && when > db->start_time) {
        when -= db->start_time;

        uint8_t num_subtimezones = *(const uint8_t *)(self->zone_start_bytes);
        const uint32_t * transition_when_array = (const uint32_t *)(self->zone_start_bytes + 2 + num_subtimezones * 12u);
        const uint8_t * transition_stz_array = (const uint8_t *)(transition_when_array + num_transitions);

        if (when > db->duration) {
            stz_idx = transition_stz_array[num_transitions - 1];

            uint32_t turnback_duration;
            stz_idx = minitz_zone_get_stz_and_turnback_duration(self, transition_stz_array, num_transitions - 1, &turnback_duration);

            // Because of the "Ban on overlapping time changes" in the spec,
            // we only need to check one previous transition for a fold.
            // So we report fold=0 or fold=1.
            fold = (when - transition_when_array[num_transitions - 1] < turnback_duration);
        } else {
            // We now know `when` will fit in a unsigned 32-bit variable.
            // On 32-bit systems, this may make it much faster to deal with.
            uint32_t when32 = (uint32_t)when;

            if (transition_when_array[0] <= when32) {
                // We are after the first transition.
                // This means we can't assume stz_idx=0 or fold=0.

                int32_t last_transition_index = 0;
                for (; last_transition_index + 1 < num_transitions; ++last_transition_index) {
                    if (transition_when_array[last_transition_index + 1] > when32) {
                        break;
                    }
                }

                uint32_t turnback_duration;
                stz_idx = minitz_zone_get_stz_and_turnback_duration(self, transition_stz_array, last_transition_index, &turnback_duration);

                // Because of the "Ban on overlapping time changes" in the spec,
                // we only need to check one previous transition for a fold.
                // So we report fold=0 or fold=1.
                fold = (when32 - transition_when_array[last_transition_index] < turnback_duration);
            }
        }
    }

    mp_obj_t tuple[4];
    minitz_zone_get_subtimezone(self, stz_idx, tuple);
    tuple[3] = mp_obj_new_int(fold);
    return mp_obj_new_tuple(4, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_2(minitz_zone_lookup_utc_obj, minitz_zone_lookup_utc);


// Zone.lookup_local(self, when: int, fold: int) -> tuple[int, str, int]
// Returns: (offset: int, designator: str, is_dst: int)
static mp_obj_t minitz_zone_lookup_local(mp_obj_t self_in, mp_obj_t when_obj, mp_obj_t fold_obj) {
    mp_obj_minitz_zone_t *self = MP_OBJ_TO_PTR(self_in);
    int64_t when = mp_obj_get_int_ll(when_obj);
    mp_int_t fold = mp_obj_get_int(fold_obj);
    if (fold != 0 && fold != 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("fold out of range"));
    }

    mp_obj_minitz_db_t *db = MP_OBJ_TO_PTR(self->db_obj);
    uint8_t num_transitions = *(const uint8_t *)(self->zone_start_bytes + 1);

    uint8_t stz_idx = 0;

    // Check if the specified time *might* be in range of the file.
    // Note that until we get the answer from this function, we don't
    // know the offset, so we don't know if the time is actually in range.
    // However, we know how large the offset might be, so we can exclude
    // local times that can't possibly be in range of this file.
    if (num_transitions != 0 && when - MINITZ_MIN_OFFSET > db->start_time) {
        when -= db->start_time;

        uint8_t num_subtimezones = *(const uint8_t *)(self->zone_start_bytes);
        const uint32_t * transition_when_array = (const uint32_t *)(self->zone_start_bytes + 2 + num_subtimezones * 12u);
        const uint8_t * transition_stz_array = (const uint8_t *)(transition_when_array + num_transitions);

        if (when - MINITZ_MAX_OFFSET > db->duration) {
            if (fold != 0) {
                // We're asking for fold=1 but we have no known folds past the end
                // of file.
                mp_raise_ValueError(MP_ERROR_TEXT("local time invalid, no fold"));
            }
            stz_idx = transition_stz_array[num_transitions - 1];
        } else {
            // We now know `when + MINITZ_MAGIC_CONSTANT` will fit in a
            // unsigned 32-bit variable.
            // On 32-bit systems, this may make it much faster to deal with.
            uint32_t when_local = (uint32_t)(when + MINITZ_MAGIC_CONSTANT);

            uint32_t old_tz_adjusted_offset = minitz_zone_get_subtimezone_adjusted_offset(self, 0);
            int32_t next_transition_index = 0;
            for (int32_t next_transition_index = 0;; ++next_transition_index) {
                if (next_transition_index >= num_transitions) {
                    if (fold != 0) {
                        // We're asking for fold=1 and there was no fold because
                        // the clocks didn't go back.
                        mp_raise_ValueError(MP_ERROR_TEXT("local time invalid, no fold"));
                    }
                    stz_idx = transition_stz_array[num_transitions - 1];
                    break;
                }
                uint32_t next_tz_adjusted_offset = minitz_zone_get_subtimezone_adjusted_offset(self, transition_stz_array[next_transition_index]);
                uint32_t transition_utc = transition_when_array[next_transition_index];
                uint32_t transition_from_local = transition_utc + old_tz_adjusted_offset;
                uint32_t transition_to_local = transition_utc + next_tz_adjusted_offset;
                if (when_local < transition_from_local && fold == 0) {
                    // Found it.
                    // Just before this transition, is the first time that particular
                    // local time ever happened.
                    if (next_transition_index != 0) {
                        stz_idx = transition_stz_array[next_transition_index - 1];
                    }
                    // else set stz_idx to 0, but it's already set to that so we don't bother.
                    break;
                }
                if (when_local < transition_to_local) {
                    if (fold == 0) {
                        // The clocks went forward, and we're asking about a time that
                        // never existed because it was skipped when the clocks went forward.
                        mp_raise_ValueError(MP_ERROR_TEXT("local time invalid due to discontinuity"));
                    } else {
                        // We're asking for fold=1 and there was no fold because the clocks
                        // didn't go back.
                        mp_raise_ValueError(MP_ERROR_TEXT("local time invalid, no fold"));
                    }
                }
                if (fold != 0 && when_local < transition_from_local) {
                    // The clocks went backward, and we're asking about a time that
                    // happened twice.  The second time is just after this transition
                    // that we just found.
                    stz_idx = transition_stz_array[next_transition_index];
                    break;
                }
                old_tz_adjusted_offset = next_tz_adjusted_offset;
            }
        }
    } else if (fold != 0) {
        // We're asking for fold=1 but we have no known folds past the beginning of file.
        mp_raise_ValueError(MP_ERROR_TEXT("local time invalid, no fold"));
    }

    mp_obj_t tuple[3];
    minitz_zone_get_subtimezone(self, stz_idx, tuple);
    return mp_obj_new_tuple(3, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_3(minitz_zone_lookup_local_obj, minitz_zone_lookup_local);

// Attributes:
//
// Zone.start_time: int
// Zone.end_time: int
// Zone.db: Database
// Zone.num_transitions: int
//
static void minitz_zone_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_obj_minitz_zone_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // Load attribute.
        if (attr == MP_QSTR_start_time) {
            mp_obj_minitz_db_t *db = MP_OBJ_TO_PTR(self->db_obj);
            dest[0] = mp_obj_new_int_from_ll(db->start_time);
        } else if (attr == MP_QSTR_end_time) {
            mp_obj_minitz_db_t *db = MP_OBJ_TO_PTR(self->db_obj);
            int64_t end_time = db->start_time + (int64_t)db->duration;
            dest[0] = mp_obj_new_int_from_ll(end_time);
        } else if (attr == MP_QSTR_db) {
            dest[0] = self->db_obj;
        } else if (attr == MP_QSTR_num_transitions) {
            uint8_t num_transitions = *(uint8_t *)(self->zone_start_bytes + 1);
            dest[0] = mp_obj_new_int(num_transitions);
        } else {
            // Continue lookup in locals_dict.
            dest[1] = MP_OBJ_SENTINEL;
        }
    }
}

static const mp_rom_map_elem_t minitz_zone_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_get_transitions), MP_ROM_PTR(&minitz_zone_get_transitions_obj)},
    { MP_ROM_QSTR(MP_QSTR_lookup_utc), MP_ROM_PTR(&minitz_zone_lookup_utc_obj)},
    { MP_ROM_QSTR(MP_QSTR_lookup_local), MP_ROM_PTR(&minitz_zone_lookup_local_obj)},
};
static MP_DEFINE_CONST_DICT(minitz_zone_locals_dict, minitz_zone_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    minitz_zone_type,
    MP_QSTR_Zone,
    MP_TYPE_FLAG_NONE,
    attr, minitz_zone_attr,
    locals_dict, &minitz_zone_locals_dict
    );


// Constructor for Database:
// Database(data: bytes) -> Database
static mp_obj_t minitz_db_make_new(const mp_obj_type_t *type_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    GET_STR_DATA_LEN(args[0], data_bytes, data_len);

    // Check size is OK:
    //
    // TzList: Minimum size: 20 byte header + 4 byte data + 4 byte CRC
    // TzIF: Minimum size: 28 byte header + 4 byte CRC
    // TzDB: Minimum size: 40 byte header + 4 byte CRC + more
    // Files are always a multiple of 4 bytes long.
    //
    // The 1GiB limit is to avoid integer overflow bugs.  (In practice,
    // platforms running this code are unlikely to have that much memory,
    // so it's probably impossible to hit.  And no valid file will ever
    // be that big.  But having an explicit limit makes it easier to
    // reason about the checks below).
    if (data_len < 20u + 4u + 4u ||
        (data_len & 3u) != 0u ||
        data_len > 1024u * 1024u * 1024u) {
        raise_minitz_data_error();
    }

    bool is_tzdb = (memcmp(data_bytes, mini_tzdb_magic, 8) == 0);
    bool is_tzif = (memcmp(data_bytes, mini_tzif_magic, 8) == 0);
    bool is_tzlist = (memcmp(data_bytes, mini_tzlist_magic, 8) == 0);
    if (!is_tzdb && !is_tzif && !is_tzlist) {
        raise_minitz_data_error();
    }

    // Assert that the pointer is correctly aligned.
    // I believe that MicroPython currently aligns "bytes" object data
    // pointers for uint64_t access.  If that belief is wrong,
    // or becomes wrong in the future, then this assert will be hit.
    assert(((uintptr_t)data_bytes & ((uintptr_t)alignof(uint64_t) - (uintptr_t)1U)) == 0);

    // Should check CRC here.  Unfortunately MicroPython doesn't
    // seem to have a good way to do that.

    int64_t start_time = 0;
    uint32_t duration = 0;

    if (!is_tzlist) {

        if (data_len < 28u + 4u ||
            (is_tzdb && data_len < 40u + 4u)) {
            raise_minitz_data_error();
        }
        // Read and sanity check the data start time and duration.
        start_time = *(int64_t *)(data_bytes + 8);
        duration = *(uint32_t *)(data_bytes + 16);
        if (start_time < 0 ||
            start_time > MINITZ_MAX_TIME ||
            duration > MINITZ_MAX_DURATION ||
            start_time + duration > MINITZ_MAX_TIME) {
            raise_minitz_data_error();
        }

        // Convert start_time to the epoch that MicroPython's 'time' module is using.
        // (This subtraction is why that field is read as a signed value, instead of
        // unsigned as specified in the MiniTZ spec).
        start_time -= TIMEUTILS_SECONDS_FROM_1970_TO_EPOCH;

        uint16_t num_leap_seconds = *(uint16_t *)(data_bytes + 22);

        if (is_tzdb) {
            uint16_t num_zones = *(uint16_t *)(data_bytes + 24);
            uint32_t file_offset_zone_names = *(uint32_t *)(data_bytes + 28);
            uint32_t file_offset_leap_seconds = *(uint32_t *)(data_bytes + 32);
            uint32_t file_offset_crc = *(uint32_t *)(data_bytes + 36);

            // Sanity check all offset fields.
            //
            // Note that the ">= data_len" checks may appear redundant,
            // since they are followed by a "stricter" check on the next line.
            // But they are not redundant, they are essential.
            // Without those checks, there is a risk of integer overflow in the
            // "stricter" check, allowing a malicious file to get through these
            // checks and cause security issues.
            // Note that the limit on data_len, enforced above, is low enough
            // that the following check cannot possibly invoke integer overflow.
            if (num_zones == 0 ||
                file_offset_crc != data_len - 4u ||
                file_offset_zone_names < 40u ||
                file_offset_zone_names >= data_len ||
                file_offset_zone_names + num_zones * 2u + 1u > file_offset_crc ||
                (file_offset_zone_names & 3u) != 0u ||
                file_offset_leap_seconds < 40u ||
                file_offset_leap_seconds >= data_len ||
                file_offset_leap_seconds + num_leap_seconds * 8u > file_offset_crc ||
                (file_offset_leap_seconds & 3u) != 0u) {
                raise_minitz_data_error();
            }
        } else {
            uint8_t num_subtimezones = *(uint8_t *)(data_bytes + 26);
            uint8_t num_transitions = *(uint8_t *)(data_bytes + 27);

            uint32_t file_offset_leap_seconds = 28u + num_subtimezones * 12u + num_transitions * 5u;
            file_offset_leap_seconds = (file_offset_leap_seconds + 3u) & ~(uint32_t)3u;
            uint32_t file_offset_crc = file_offset_leap_seconds + num_leap_seconds * 8u;
            
            if (num_subtimezones == 0 ||
                file_offset_crc != data_len - 4u) {
                raise_minitz_data_error();
            }
        }
    }
    
    mp_obj_minitz_db_t *self = mp_obj_malloc(mp_obj_minitz_db_t, type_in);
    
    self->data_obj = args[0];
    self->data_bytes = data_bytes;
    self->data_len = data_len;
    self->start_time = start_time;
    self->duration = duration;

    return MP_OBJ_FROM_PTR(self);
}

// Check if this is a full timezone database file
static uint16_t minitz_db_is_tzdb(mp_obj_minitz_db_t *self)
{
    return self->data_bytes[4] == 'D';
}

// Check if this is a single-timezone TZIF file
static uint16_t minitz_db_is_tzif(mp_obj_minitz_db_t *self)
{
    return self->data_bytes[4] == 'i';
}

// Check if this is a timezone names list only
static uint16_t minitz_db_is_tzlist(mp_obj_minitz_db_t *self)
{
    return self->data_bytes[4] == 'L';
}

static uint16_t minitz_db_num_zones(mp_obj_minitz_db_t *self)
{
    if (minitz_db_is_tzdb(self)) {
        return *(uint16_t *)(self->data_bytes + 24);
    } else if (minitz_db_is_tzif(self)) {
        return 1;
    } else {
        // tzlist
        return *(uint16_t *)(self->data_bytes + 8);
    }
}

// Attributes:
//
// Database.start_time: int  (int64 relative to the epoch used by the time module)
// Database.end_time: int  (int64 relative to the epoch used by the time module)
// Database.num_zones: int  (uint8 range)
// Database.kind: int  (1=tzif, 2=db, 3=list)
// Database.crc: int  (uint32 range)
//
static void minitz_db_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_obj_minitz_db_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // Load attribute.
        if (attr == MP_QSTR_start_time && !minitz_db_is_tzlist(self)) {
            dest[0] = mp_obj_new_int_from_ll(self->start_time);
        } else if (attr == MP_QSTR_end_time && !minitz_db_is_tzlist(self)) {
            int64_t end_time = self->start_time + (int64_t)self->duration;
            dest[0] = mp_obj_new_int_from_ll(end_time);
        } else if (attr == MP_QSTR_num_zones) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(minitz_db_num_zones(self));
        } else if (attr == MP_QSTR_kind) {
            if (minitz_db_is_tzif(self)) {
                dest[0] = MP_OBJ_NEW_SMALL_INT(1);  // tzif
            }
            else if (minitz_db_is_tzdb(self)) {
                dest[0] = MP_OBJ_NEW_SMALL_INT(2);  // tzdb
            }
            else {
                dest[0] = MP_OBJ_NEW_SMALL_INT(3);  // tzlist
            }
        } else if (attr == MP_QSTR_crc) {
            uint32_t crc = *(uint32_t *)(self->data_bytes + self->data_len - 4);
            dest[0] = mp_obj_new_int_from_uint32(crc);
        } else {
            // Continue lookup in locals_dict.
            dest[1] = MP_OBJ_SENTINEL;
        }
    }
}

// Database.get_zone(self, index: int) -> Zone
static mp_obj_t minitz_db_get_zone(mp_obj_t self_in, mp_obj_t index_obj) {
    mp_obj_minitz_db_t *self = MP_OBJ_TO_PTR(self_in);
    mp_uint_t index = mp_obj_get_uint(index_obj);
    mp_uint_t num_zones = minitz_db_num_zones(self);

    if (index >= num_zones) {
        mp_raise_ValueError(MP_ERROR_TEXT("Bad index"));
    }
    if (minitz_db_is_tzlist(self)) {
        mp_raise_ValueError(MP_ERROR_TEXT("File only contains zone names"));
    }

    const byte *zone_start_bytes;
    uint32_t file_offset_zone_data;
    bool is_tzdb = minitz_db_is_tzdb(self);
    if (is_tzdb) {
        file_offset_zone_data = ((uint32_t *)(self->data_bytes + 36))[index];
        if (file_offset_zone_data < 40 ||
            file_offset_zone_data > self->data_len ||
            file_offset_zone_data + (14u + 4u) > self->data_len ||
            (file_offset_zone_data & 3u) != 2u) {
            raise_minitz_data_error();
        }

        zone_start_bytes = self->data_bytes + file_offset_zone_data;
    } else {
        zone_start_bytes = self->data_bytes + 26;
    }

    uint8_t num_subtimezones = *(uint8_t *)(zone_start_bytes);
    uint8_t num_transitions = *(uint8_t *)(zone_start_bytes + 1);

    if (is_tzdb)
    {
        uint32_t zone_len = 2u + num_subtimezones * 12u + num_transitions * 5u;
        if (num_subtimezones == 0 ||
            file_offset_zone_data + zone_len > self->data_len) {
            raise_minitz_data_error();
        }
    }

    // We error-check all the details here, rather than in every accessor.
    // This keeps the code size and complexity of the accessor
    // functions down.
    // It may also improves the speed of the accessor functions,
    // which may be called a lot.
    //
    // Note that for MiniTzDb files, we don't want to check every zone
    // when the MiniTzDb file is loaded.  That's a lot of pointless work,
    // given that only a few of the zones will ever be used.  So instead
    // we check here, when the zone object is created.

    const uint32_t * transition_when_array = (const uint32_t *)(zone_start_bytes + 2 + num_subtimezones * 12u);
    const uint8_t * transition_stz_array = (const uint8_t *)(transition_when_array + num_transitions);

    for (int stz_idx = 0; stz_idx < num_subtimezones; stz_idx++) {
        const byte * stz_data = zone_start_bytes + 2 + stz_idx * 12u;
        int designator_len = strnlen(stz_data, 7);
        uint8_t is_dst = stz_data[7];
        int32_t offset = *(int32_t *)(stz_data + 8);

        if (designator_len > 6 ||
            is_dst > 1 ||
            offset < MINITZ_MIN_OFFSET ||
            offset > MINITZ_MAX_OFFSET) {
            raise_minitz_data_error();
        }
    }

    uint32_t must_be_after = 0;
    int32_t last_offset = *(int32_t *)(zone_start_bytes + 10);
    for (int transition_idx = 0; transition_idx < num_transitions; transition_idx++) {
        uint8_t stz_idx = transition_stz_array[index];
        uint32_t when = transition_when_array[index];
        // It might appear that the comparison of `when` against `duration`
        // here is unnecessary - it looks like we could just check `must_be_after`
        // against `duration` after the loop.  However, it is actually
        // essential, because it guards against integer overflow in the
        // "overlapping time changes" detection code.
        if (stz_idx >= num_subtimezones ||
            when >= self->duration) {
            raise_minitz_data_error();
        }

        // Detect "overlapping time changes" and block them.
        // (Also checks that the `when` array is sorted, since that happens
        // as a natural part of this check, it's not necessary to do it separately).
        //
        // This is an optimized implementation of the algorithm in the spec.
        int32_t next_offset = *(int32_t *)(zone_start_bytes + 10 + stz_idx * 12u);
        int32_t turnback = last_offset - next_offset;
        if (turnback < 0) {
            turnback = 0;
        }
        // Must skip this test for first transition.  This is because the
        // file start time is NOT a transition, so we CAN turn local time
        // back to before the local time at file start.
        // We do want to check that the first transition has `when>0`
        // though, so we keep that part of the test, we just don't add
        // `turnback`.
        if (transition_idx != 0) {
            must_be_after += turnback;
        }
        if (when <= must_be_after) {
            raise_minitz_data_error();
        }

        // We do `must_be_after = when + turnback` here, then next time through
        // we will calculate `must_be_after + turnback`.  In other words,
        // using T1 to mean the first transition and T2 to mean the next
        // transition,  we calculate `T1_when + T1_turnback + T2_turnback`.
        //
        // That's one less than the `X` defined in the spec.  The spec says
        // that `when == X` is not an error, so we get the same effect by
        // comparing against `X - 1` and raising an error in the
        // `when == X - 1` case.
        //
        // Note that this WILL fit in a 32-bit unsigned integer.
        // This is because `(T1_turnback + T2_turnback)` is either:
        //  * `(prev_offset - T1_offset) + (T1_offset - T2_offset)`,
        //    which is equal to `(prev_offset - T2_offset)`, or
        //  * `(prev_offset - T1_offset) + 0`,
        //    which is equal to `(prev_offset - T1_offset)`, or
        //  * `0 + (T1_offset - T2_offset)`,
        //    which is equal to `(T1_offset - T2_offset)`
        // So in all those cases it is the result of subtracting two offsets,
        // so it cannot be more than `(MINITZ_MAX_OFFSET - MINITZ_MIN_OFFSET)`.
        // And `when` is limited by `duration` which has a hard limit that is
        // small enough that everything will fit.
        must_be_after = when + turnback;
        last_offset = next_offset;
    }

    mp_obj_minitz_zone_t *zone = mp_obj_malloc(mp_obj_minitz_zone_t, &minitz_zone_type);
    
    zone->db_obj = self_in;
    zone->zone_start_bytes = zone_start_bytes;

    return MP_OBJ_FROM_PTR(zone);
}
static MP_DEFINE_CONST_FUN_OBJ_2(minitz_db_get_zone_obj, minitz_db_get_zone);

// Database.get_zone_names(self, start: int, end: int | None) -> list[str]
//
// Returns a slice of the zone names array.
//
// Negative values of `start` or `end` mean to count from the end.
// A None value for `end` means to the end.
//
static mp_obj_t minitz_db_get_zone_names(mp_obj_t self_in, mp_obj_t start_obj, mp_obj_t end_obj) {
    mp_obj_minitz_db_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t num_zones = minitz_db_num_zones(self);

    mp_int_t start;
    mp_int_t end;
    minitz_parse_slice_like(num_zones, start_obj, end_obj, &start, &end);

    mp_obj_list_t *list = MP_OBJ_TO_PTR(mp_obj_new_list(end - start, NULL));

    // Check there is at least 1 piece of data to return.
    // The following can be very expensive if `end` is large,
    // so we don't want to do it if we're not going to use any of it.
    if (end > start) {
        if (minitz_db_is_tzif(self)) {
            // num_zones is 1 so there can only be one entry, at most.
            // And we already checked there is at least one entry.
            // So there is exactly one entry.
            // ... And it doesn't have a name!  So we hardcode something.
            list->items[0] = mp_obj_new_str_0("tzif");
        } else {
            const char *cur;
            int32_t remaining_size;
            if (minitz_db_is_tzdb(self)) {
                uint32_t file_offset_zone_names = *(uint32_t *)(self->data_bytes + 28);
                uint32_t file_offset_crc = *(uint32_t *)(self->data_bytes + 36);
                cur = self->data_bytes + file_offset_zone_names;
                remaining_size = file_offset_crc - file_offset_zone_names;
            } else {
                // tzlist
                cur = self->data_bytes + 16;
                remaining_size = self->data_len - (16 + 4);
            }
            for (mp_int_t ii = 0; ii < end; ++ii) {
                int32_t len = strnlen(cur, remaining_size);
                if (len == 0 || len + 1 >= remaining_size) {
                    raise_minitz_data_error();
                }
                if (ii >= start) {
                    list->items[ii - start] = mp_obj_new_str(cur, len);
                }
                cur += len + 1;
                remaining_size -= len + 1;
            }
            // If we're at the end of the data, check the end-of-data
            // marker is present.
            if (end == num_zones && *cur != 0) {
                raise_minitz_data_error();
            }
        }
    }

    return MP_OBJ_FROM_PTR(list);
}
static MP_DEFINE_CONST_FUN_OBJ_3(minitz_db_get_zone_names_obj, minitz_db_get_zone_names);

// Database.find_zone_index(self, zone_name: str) -> int
//
// Returns -1 if not found.
//
// Note that the implementation of get_zone_by_name() relies on the fact
// that this always returns a small int.
static mp_obj_t minitz_db_find_zone_index(mp_obj_t self_in, mp_obj_t zone_name) {
    mp_obj_minitz_db_t *self = MP_OBJ_TO_PTR(self_in);

    GET_STR_DATA_LEN(zone_name, name_bytes, name_len);

    if (minitz_db_is_tzif(self)) {
        // There's only one entry.
        // So either we always fail, or always succeed, or maybe compare against
        // the string constant that get_zone_names() returns?  Not clear what the
        // best answer is here.  For now, always succeed.
        return MP_OBJ_NEW_SMALL_INT(0);
    }

    const char *cur;
    int32_t remaining_size;
    mp_int_t num_zones = minitz_db_num_zones(self);
    if (minitz_db_is_tzdb(self)) {
        uint32_t file_offset_zone_names = *(uint32_t *)(self->data_bytes + 28);
        uint32_t file_offset_crc = *(uint32_t *)(self->data_bytes + 36);
        cur = self->data_bytes + file_offset_zone_names;
        remaining_size = file_offset_crc - file_offset_zone_names;
    } else {
        // tzlist
        cur = self->data_bytes + 16;
        remaining_size = self->data_len - (16 + 4);
    }

    for (mp_int_t ii = 0; ii < num_zones; ++ii) {
        int32_t len = strnlen(cur, remaining_size);
        if (len == 0 || len + 1 >= remaining_size) {
            raise_minitz_data_error();
        }
        if (len == name_len && memcmp(cur, name_bytes, name_len) == 0) {
            return MP_OBJ_NEW_SMALL_INT(ii);
        }
        cur += len + 1;
        remaining_size -= len + 1;
    }
    // We're at the end of the data, check the end-of-data
    // marker is present.
    if (*cur != 0) {
        raise_minitz_data_error();
    }

    // Not found
    return MP_OBJ_NEW_SMALL_INT(-1);
}
static MP_DEFINE_CONST_FUN_OBJ_2(minitz_db_find_zone_index_obj, minitz_db_find_zone_index);

// Database.get_zone_by_name(self, zone_name: str) -> int
static mp_obj_t minitz_db_get_zone_by_name(mp_obj_t self_in, mp_obj_t zone_name) {
    mp_obj_t index_obj = minitz_db_find_zone_index(self_in, zone_name);
    mp_int_t index = MP_OBJ_SMALL_INT_VALUE(index_obj);

    if (index < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("requested timezone not found"));
    }

    return minitz_db_get_zone(self_in, index_obj);
}
static MP_DEFINE_CONST_FUN_OBJ_2(minitz_db_get_zone_by_name_obj, minitz_db_get_zone_by_name);

static const mp_rom_map_elem_t minitz_db_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_get_zone), MP_ROM_PTR(&minitz_db_get_zone_obj)},
    { MP_ROM_QSTR(MP_QSTR_get_zone_names), MP_ROM_PTR(&minitz_db_get_zone_names_obj)},
    { MP_ROM_QSTR(MP_QSTR_find_zone_index), MP_ROM_PTR(&minitz_db_find_zone_index)},
    { MP_ROM_QSTR(MP_QSTR_get_zone_by_name), MP_ROM_PTR(&minitz_db_get_zone_by_name_obj)},
    // { MP_ROM_QSTR(MP_QSTR_get_tai_utc), MP_ROM_PTR(&minitz_db_get_tai_utc)},
};
static MP_DEFINE_CONST_DICT(minitz_db_locals_dict, minitz_db_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    minitz_db_type,
    MP_QSTR_Database,
    MP_TYPE_FLAG_NONE,
    make_new, minitz_db_make_new,
    attr, minitz_db_attr,
    locals_dict, &minitz_db_locals_dict
    );

static const mp_rom_map_elem_t mp_module_minitz_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__minitz) },

    { MP_ROM_QSTR(MP_QSTR_Database), MP_ROM_PTR(&minitz_db_type) },
};
static MP_DEFINE_CONST_DICT(mp_module_minitz_globals, mp_module_minitz_globals_table);

const mp_obj_module_t mp_module_minitz = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_minitz_globals,
};

MP_REGISTER_EXTENSIBLE_MODULE(MP_QSTR__minitz, mp_module_minitz);

#endif // MICROPY_PY_MINITZ
