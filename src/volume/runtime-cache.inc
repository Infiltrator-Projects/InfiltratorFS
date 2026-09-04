// SPDX-License-Identifier: GPL-3.0-or-later
#define INFS_DIRECTORY_CACHE_SLOTS 65536u
#define INFS_DIRECTORY_STATE_SLOTS 8192u
#define INFS_DIRECTORY_PAGE_UNKNOWN UINT32_MAX

struct infs_directory_cache_entry {
    uint8_t parent_id[16];
    uint8_t object_id[16];
    uint64_t hash;
    uint32_t page_index;
    uint16_t type;
    uint16_t name_length;
    uint8_t state;
    char name[INFS_NAME_MAX + 1u];
};

struct infs_directory_cache_state {
    uint8_t parent_id[16];
    uint8_t state;
    uint8_t complete;
};

static uint64_t directory_cache_hash(const uint8_t parent_id[16],
                                     const char *name, size_t length)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (unsigned i = 0; i < 16; ++i) {
        h ^= parent_id[i];
        h *= UINT64_C(1099511628211);
    }
    for (size_t i = 0; i < length; ++i) {
        h ^= (uint8_t)name[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static void directory_cache_clear(struct infs_volume *vol)
{
    if (!vol)
        return;
    if (vol->directory_cache)
        memset(vol->directory_cache, 0,
               vol->directory_cache_slots * sizeof(*vol->directory_cache));
    if (vol->directory_cache_states)
        memset(vol->directory_cache_states, 0,
               vol->directory_cache_state_slots *
               sizeof(*vol->directory_cache_states));
}

static void directory_cache_init(struct infs_volume *vol)
{
    if (!vol || vol->directory_cache)
        return;
    vol->directory_cache = calloc(INFS_DIRECTORY_CACHE_SLOTS,
                                  sizeof(*vol->directory_cache));
    vol->directory_cache_states = calloc(INFS_DIRECTORY_STATE_SLOTS,
                                         sizeof(*vol->directory_cache_states));
    if (!vol->directory_cache || !vol->directory_cache_states) {
        free(vol->directory_cache);
        free(vol->directory_cache_states);
        vol->directory_cache = NULL;
        vol->directory_cache_states = NULL;
        return;
    }
    vol->directory_cache_slots = INFS_DIRECTORY_CACHE_SLOTS;
    vol->directory_cache_state_slots = INFS_DIRECTORY_STATE_SLOTS;
}

static void directory_cache_destroy(struct infs_volume *vol)
{
    if (!vol)
        return;
    free(vol->directory_cache);
    free(vol->directory_cache_states);
    vol->directory_cache = NULL;
    vol->directory_cache_states = NULL;
    vol->directory_cache_slots = 0;
    vol->directory_cache_state_slots = 0;
}

static struct infs_directory_cache_state *directory_cache_state(
    struct infs_volume *vol, const uint8_t parent_id[16], int create)
{
    if (!vol || !vol->directory_cache_states ||
        !vol->directory_cache_state_slots)
        return NULL;
    size_t mask = vol->directory_cache_state_slots - 1u;
    size_t slot = (size_t)(object_cache_hash(parent_id) & mask);
    for (size_t probe = 0; probe < vol->directory_cache_state_slots; ++probe) {
        struct infs_directory_cache_state *state =
            &vol->directory_cache_states[(slot + probe) & mask];
        if (!state->state) {
            if (!create)
                return NULL;
            memcpy(state->parent_id, parent_id, 16);
            state->state = 1;
            return state;
        }
        if (memcmp(state->parent_id, parent_id, 16) == 0)
            return state;
    }
    return NULL;
}

static int directory_cache_complete(struct infs_volume *vol,
                                    const uint8_t parent_id[16])
{
    struct infs_directory_cache_state *state =
        directory_cache_state(vol, parent_id, 0);
    return state && state->complete;
}

static void directory_cache_mark_complete(struct infs_volume *vol,
                                          const uint8_t parent_id[16])
{
    struct infs_directory_cache_state *state =
        directory_cache_state(vol, parent_id, 1);
    if (state)
        state->complete = 1;
}

static void directory_cache_mark_incomplete(struct infs_volume *vol,
                                            const uint8_t parent_id[16])
{
    struct infs_directory_cache_state *state =
        directory_cache_state(vol, parent_id, 0);
    if (state)
        state->complete = 0;
}

static struct infs_directory_cache_entry *directory_cache_find(
    struct infs_volume *vol, const uint8_t parent_id[16], const char *name)
{
    if (!vol || !vol->directory_cache || !name)
        return NULL;
    size_t length = strlen(name);
    uint64_t h = directory_cache_hash(parent_id, name, length);
    size_t mask = vol->directory_cache_slots - 1u;
    size_t slot = (size_t)(h & mask);
    for (size_t probe = 0; probe < vol->directory_cache_slots; ++probe) {
        struct infs_directory_cache_entry *entry =
            &vol->directory_cache[(slot + probe) & mask];
        if (entry->state == 0)
            return NULL;
        if (entry->state == 1 && entry->hash == h &&
            entry->name_length == length &&
            memcmp(entry->parent_id, parent_id, 16) == 0 &&
            memcmp(entry->name, name, length) == 0)
            return entry;
    }
    return NULL;
}

static int directory_cache_lookup(struct infs_volume *vol,
                                  const uint8_t parent_id[16], const char *name,
                                  struct infs_dirent_disk *entry_out)
{
    struct infs_directory_cache_entry *entry =
        directory_cache_find(vol, parent_id, name);
    if (!entry)
        return 0;
    if (entry_out) {
        memset(entry_out, 0, sizeof(*entry_out));
        entry_out->object_type = infs_cpu_to_le16(entry->type);
        memcpy(entry_out->object_id, entry->object_id, 16);
    }
    return 1;
}

static int directory_cache_page(struct infs_volume *vol,
                                const uint8_t parent_id[16], const char *name,
                                uint32_t *page_out)
{
    struct infs_directory_cache_entry *entry =
        directory_cache_find(vol, parent_id, name);
    if (!entry || entry->page_index == INFS_DIRECTORY_PAGE_UNKNOWN)
        return 0;
    if (page_out)
        *page_out = entry->page_index;
    return 1;
}

static int directory_cache_store(struct infs_volume *vol,
                                 const uint8_t parent_id[16], const char *name,
                                 const uint8_t object_id[16], uint16_t type,
                                 uint32_t page_index)
{
    if (!vol || !vol->directory_cache || !name)
        return 0;
    size_t length = strlen(name);
    if (!length || length > INFS_NAME_MAX)
        return 0;
    uint64_t h = directory_cache_hash(parent_id, name, length);
    size_t mask = vol->directory_cache_slots - 1u;
    size_t slot = (size_t)(h & mask);
    size_t tombstone = SIZE_MAX;
    for (size_t probe = 0; probe < vol->directory_cache_slots; ++probe) {
        size_t at = (slot + probe) & mask;
        struct infs_directory_cache_entry *entry = &vol->directory_cache[at];
        if (entry->state == 1 && entry->hash == h &&
            entry->name_length == length &&
            memcmp(entry->parent_id, parent_id, 16) == 0 &&
            memcmp(entry->name, name, length) == 0) {
            memcpy(entry->object_id, object_id, 16);
            entry->type = type;
            if (page_index != INFS_DIRECTORY_PAGE_UNKNOWN)
                entry->page_index = page_index;
            return 1;
        }
        if (entry->state == 2 && tombstone == SIZE_MAX)
            tombstone = at;
        if (entry->state != 0)
            continue;
        if (tombstone != SIZE_MAX)
            entry = &vol->directory_cache[tombstone];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->parent_id, parent_id, 16);
        memcpy(entry->object_id, object_id, 16);
        memcpy(entry->name, name, length);
        entry->name[length] = '\0';
        entry->hash = h;
        entry->type = type;
        entry->name_length = (uint16_t)length;
        entry->page_index = page_index;
        entry->state = 1;
        return 1;
    }
    directory_cache_mark_incomplete(vol, parent_id);
    return 0;
}

static void directory_cache_remove(struct infs_volume *vol,
                                   const uint8_t parent_id[16], const char *name)
{
    struct infs_directory_cache_entry *entry =
        directory_cache_find(vol, parent_id, name);
    if (entry)
        entry->state = 2;
}

static void directory_cache_invalidate_parent(struct infs_volume *vol,
                                              const uint8_t parent_id[16])
{
    if (!vol || !vol->directory_cache)
        return;
    for (size_t i = 0; i < vol->directory_cache_slots; ++i) {
        struct infs_directory_cache_entry *entry = &vol->directory_cache[i];
        if (entry->state == 1 &&
            memcmp(entry->parent_id, parent_id, 16) == 0)
            entry->state = 2;
    }
    directory_cache_mark_incomplete(vol, parent_id);
}
