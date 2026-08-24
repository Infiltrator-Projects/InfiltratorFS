// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/forensic.h"

#include "infilfs/fs.h"
#include "infilfs/posix_io.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct output_context {
    int jsonl;
};

static const char *kind_name(uint32_t kind)
{
    switch (kind) {
    case INFS_FORENSIC_CHECKPOINT: return "checkpoint";
    case INFS_FORENSIC_OBJECT: return "object";
    case INFS_FORENSIC_DIRECTORY_PAGE: return "directory-page";
    case INFS_FORENSIC_INDEX_PAGE: return "index-page";
    default: return "unknown";
    }
}

static const char *state_name(uint32_t state)
{
    switch (state) {
    case INFS_FORENSIC_STATE_CURRENT: return "current";
    case INFS_FORENSIC_STATE_STALE: return "stale";
    case INFS_FORENSIC_STATE_ORPHANED: return "orphaned";
    default: return "unknown";
    }
}

static const char *object_type_name(uint16_t type)
{
    switch (type) {
    case INFS_OBJECT_DIRECTORY: return "directory";
    case INFS_OBJECT_FILE: return "file";
    case INFS_OBJECT_SYMLINK: return "symlink";
    case INFS_OBJECT_INDEX: return "index";
    case INFS_OBJECT_CHECKSUM: return "checksum";
    default: return "none";
    }
}

static infs_status print_record(const struct infs_forensic_record *record,
                                void *opaque)
{
    struct output_context *context = opaque;
    char object_id[37];
    char parent_id[37];
    infs_uuid_to_string(record->object_id, object_id);
    infs_uuid_to_string(record->parent_id, parent_id);

    if (context->jsonl) {
        printf("{\"record\":\"metadata\",\"block\":%" PRIu64
               ",\"state\":\"%s\",\"kind\":\"%s\""
               ",\"generation\":%" PRIu64
               ",\"object_type\":\"%s\",\"object_version\":%u"
               ",\"object_id\":\"%s\",\"parent_id\":\"%s\""
               ",\"payload_size\":%u,\"entry_count\":%u"
               ",\"bytes_used\":%u}\n",
               record->block, state_name(record->state),
               kind_name(record->kind), record->generation,
               object_type_name(record->object_type),
               (unsigned)record->object_version, object_id, parent_id,
               record->payload_size, record->entry_count,
               record->bytes_used);
    } else {
        printf("%" PRIu64 "\t%s\t%s\t%" PRIu64 "\t%s\t%u\t%s\t%s\t%u\t%u\t%u\n",
               record->block, state_name(record->state),
               kind_name(record->kind), record->generation,
               object_type_name(record->object_type),
               (unsigned)record->object_version, object_id, parent_id,
               record->payload_size, record->entry_count,
               record->bytes_used);
    }
    return ferror(stdout) ? INFS_STATUS_IO_ERROR : INFS_STATUS_OK;
}

static void print_summary(const struct infs_forensic_summary *summary,
                          int jsonl)
{
    if (jsonl) {
        printf("{\"record\":\"summary\",\"total_blocks\":%" PRIu64
               ",\"scanned_blocks\":%" PRIu64
               ",\"records_found\":%" PRIu64
               ",\"current_records\":%" PRIu64
               ",\"stale_records\":%" PRIu64
               ",\"orphaned_records\":%" PRIu64
               ",\"unknown_records\":%" PRIu64
               ",\"checkpoints\":%" PRIu64
               ",\"objects\":%" PRIu64
               ",\"directory_pages\":%" PRIu64
               ",\"index_pages\":%" PRIu64
               ",\"current_generation\":%" PRIu64
               ",\"allocation_map_available\":%s}\n",
               summary->total_blocks, summary->scanned_blocks,
               summary->records_found, summary->current_records,
               summary->stale_records, summary->orphaned_records,
               summary->unknown_records, summary->checkpoints_found,
               summary->objects_found, summary->directory_pages_found,
               summary->index_pages_found, summary->current_generation,
               summary->allocation_map_available ? "true" : "false");
    } else {
        printf("summary\tblocks=%" PRIu64 "\trecords=%" PRIu64
               "\tcurrent=%" PRIu64 "\tstale=%" PRIu64
               "\torphaned=%" PRIu64 "\tunknown=%" PRIu64
               "\tgeneration=%" PRIu64 "\n",
               summary->scanned_blocks, summary->records_found,
               summary->current_records, summary->stale_records,
               summary->orphaned_records, summary->unknown_records,
               summary->current_generation);
    }
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--jsonl] <image-or-device>\n", program);
}

int main(int argc, char **argv)
{
    int jsonl = 0;
    const char *path = NULL;
    if (argc == 2) {
        path = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "--jsonl") == 0) {
        jsonl = 1;
        path = argv[2];
    } else {
        usage(argv[0]);
        return 2;
    }

    struct infs_storage storage;
    infs_status status = infs_posix_storage_open(&storage, path, 0);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-forensic: open: %s\n",
                infs_status_string(status));
        return 1;
    }

    if (!jsonl)
        puts("block\tstate\tkind\tgeneration\tobject-type\tversion\tobject-id\tparent-id\tpayload-size\tentries\tbytes");
    struct output_context context = { .jsonl = jsonl };
    struct infs_forensic_summary summary;
    status = infs_forensic_scan(&storage, print_record, &context, &summary);
    infs_storage_close(&storage);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-forensic: scan: %s\n",
                infs_status_string(status));
        return 1;
    }
    print_summary(&summary, jsonl);
    return ferror(stdout) ? 1 : 0;
}
