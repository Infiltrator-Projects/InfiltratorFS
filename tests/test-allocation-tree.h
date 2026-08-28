/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILFS_TEST_ALLOCATION_TREE_H
#define INFILFS_TEST_ALLOCATION_TREE_H

#define TEST_ALLOCATION_ROOT_BLOCK UINT64_C(1)
#define TEST_ALLOCATION_LEVEL2_BLOCK UINT64_C(4)
#define TEST_ALLOCATION_LEVEL1_BLOCK UINT64_C(5)
#define TEST_ALLOCATION_LEAF_BLOCK UINT64_C(6)
#define TEST_ALLOCATION_PAGE_COUNT UINT64_C(4)

static void test_allocation_bit_set(uint8_t *bitmap, uint64_t block)
{
    bitmap[block >> 3] |= (uint8_t)(1u << (block & 7u));
}

static void test_write_single_leaf_allocation_tree(
    uint8_t *image, uint64_t total_blocks, uint64_t generation,
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT],
    const uint64_t *extra_allocated, size_t extra_count)
{
    uint8_t block[INFS_BLOCK_SIZE];
    uint8_t bits[INFS_ALLOCATION_PAGE_DATA_SIZE];
    struct infs_allocation_page_disk *page;
    uint64_t pointer;
    uint32_t bytes;

    if (!image || !total_blocks ||
        total_blocks > INFS_ALLOCATION_BITS_PER_LEAF) {
        fprintf(stderr, "test allocation-tree geometry unsupported\n");
        exit(1);
    }

    memset(bits, 0, sizeof(bits));
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        test_allocation_bit_set(bits, checkpoints[i]);
    test_allocation_bit_set(bits, TEST_ALLOCATION_ROOT_BLOCK);
    test_allocation_bit_set(bits, TEST_ALLOCATION_LEVEL2_BLOCK);
    test_allocation_bit_set(bits, TEST_ALLOCATION_LEVEL1_BLOCK);
    test_allocation_bit_set(bits, TEST_ALLOCATION_LEAF_BLOCK);
    for (size_t i = 0; i < extra_count; ++i)
        test_allocation_bit_set(bits, extra_allocated[i]);

    bytes = (uint32_t)((total_blocks + 7u) / 8u);
    if (infs_allocation_page_init(
            block, INFS_ALLOCATION_LEAF_PAGE_MAGIC, generation, 0u, 0u) !=
        INFS_STATUS_OK) {
        fprintf(stderr, "test allocation leaf init failed\n");
        exit(1);
    }
    page = (struct infs_allocation_page_disk *)block;
    page->entry_count = infs_cpu_to_le32((uint32_t)total_blocks);
    page->bytes_used = infs_cpu_to_le32(bytes);
    memcpy(page + 1, bits, bytes);
    if (infs_allocation_page_finalize(block) != INFS_STATUS_OK) {
        fprintf(stderr, "test allocation leaf finalize failed\n");
        exit(1);
    }
    memcpy(image + TEST_ALLOCATION_LEAF_BLOCK * INFS_BLOCK_SIZE,
           block, sizeof(block));

    pointer = infs_cpu_to_le64(TEST_ALLOCATION_LEAF_BLOCK);
    if (infs_allocation_page_init(
            block, INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation, 0u, 1u) !=
        INFS_STATUS_OK) {
        fprintf(stderr, "test allocation level-1 init failed\n");
        exit(1);
    }
    page = (struct infs_allocation_page_disk *)block;
    page->entry_count = infs_cpu_to_le32(1u);
    page->bytes_used = infs_cpu_to_le32(sizeof(pointer));
    memcpy(page + 1, &pointer, sizeof(pointer));
    if (infs_allocation_page_finalize(block) != INFS_STATUS_OK) {
        fprintf(stderr, "test allocation level-1 finalize failed\n");
        exit(1);
    }
    memcpy(image + TEST_ALLOCATION_LEVEL1_BLOCK * INFS_BLOCK_SIZE,
           block, sizeof(block));

    pointer = infs_cpu_to_le64(TEST_ALLOCATION_LEVEL1_BLOCK);
    if (infs_allocation_page_init(
            block, INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation, 0u, 2u) !=
        INFS_STATUS_OK) {
        fprintf(stderr, "test allocation level-2 init failed\n");
        exit(1);
    }
    page = (struct infs_allocation_page_disk *)block;
    page->entry_count = infs_cpu_to_le32(1u);
    page->bytes_used = infs_cpu_to_le32(sizeof(pointer));
    memcpy(page + 1, &pointer, sizeof(pointer));
    if (infs_allocation_page_finalize(block) != INFS_STATUS_OK) {
        fprintf(stderr, "test allocation level-2 finalize failed\n");
        exit(1);
    }
    memcpy(image + TEST_ALLOCATION_LEVEL2_BLOCK * INFS_BLOCK_SIZE,
           block, sizeof(block));

    pointer = infs_cpu_to_le64(TEST_ALLOCATION_LEVEL2_BLOCK);
    if (infs_allocation_page_init(
            block, INFS_ALLOCATION_BRANCH_PAGE_MAGIC, generation, 0u,
            INFS_ALLOCATION_TREE_ROOT_LEVEL) != INFS_STATUS_OK) {
        fprintf(stderr, "test allocation root init failed\n");
        exit(1);
    }
    page = (struct infs_allocation_page_disk *)block;
    page->entry_count = infs_cpu_to_le32(1u);
    page->bytes_used = infs_cpu_to_le32(sizeof(pointer));
    memcpy(page + 1, &pointer, sizeof(pointer));
    if (infs_allocation_page_finalize(block) != INFS_STATUS_OK) {
        fprintf(stderr, "test allocation root finalize failed\n");
        exit(1);
    }
    memcpy(image + TEST_ALLOCATION_ROOT_BLOCK * INFS_BLOCK_SIZE,
           block, sizeof(block));
}

#endif
