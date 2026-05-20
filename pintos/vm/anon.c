/* anon.c: 디스크 이미지에 연결되지 않은 페이지, 즉 익명 페이지 구현. */

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "lib/string.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)
#define SWAP_SLOT_NONE   ((size_t) -1)

/* 아래 줄은 수정하지 말 것. */
static struct disk *swap_disk;
static struct bitmap *swap_bitmap;
static struct lock swap_lock;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* 이 구조체는 수정하지 말 것. */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

// 익명 페이지용 swap disk 초기화 (디스크 공간 계산)
void
vm_anon_init (void) {
	swap_disk = disk_get (1, 1);

	ASSERT (swap_disk != NULL);

	swap_bitmap = bitmap_create (disk_size (swap_disk) / SECTORS_PER_PAGE);
	lock_init (&swap_lock);
}

// 익명 페이지 초기화
bool
anon_initializer (struct page *page, enum vm_type type, void *kva UNUSED) {
	RETURN_VALUE_IF (page == NULL, false);
	page->operations = &anon_ops;
	page->anon.type = type;
	page->anon.swapped = false;
	page->anon.swap_slot = SWAP_SLOT_NONE;
	return true;
}

bool
anon_copy_page (struct page *dst, struct page *src) {
	RETURN_VALUE_IF (dst == NULL || src == NULL, false);
	RETURN_VALUE_IF (dst->frame == NULL || dst->frame->kva == NULL, false);

	if (src->frame != NULL && src->frame->kva != NULL) {
		memcpy (dst->frame->kva, src->frame->kva, PGSIZE);
		return true;
	}

	RETURN_VALUE_IF (!src->anon.swapped, false);
	RETURN_VALUE_IF (swap_disk == NULL || swap_bitmap == NULL, false);
	RETURN_VALUE_IF (src->anon.swap_slot == SWAP_SLOT_NONE, false);

	lock_acquire (&swap_lock);
	for (size_t i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_read (swap_disk, src->anon.swap_slot * SECTORS_PER_PAGE + i,
		           (uint8_t *) dst->frame->kva + DISK_SECTOR_SIZE * i);
	}
	lock_release (&swap_lock);
	return true;
}

// swap in
static bool
anon_swap_in (struct page *page, void *kva) {
	RETURN_VALUE_IF (page == NULL || kva == NULL, false);

	if (page->anon.swapped) {
		RETURN_VALUE_IF (swap_disk == NULL || swap_bitmap == NULL, false);
		RETURN_VALUE_IF (page->anon.swap_slot == SWAP_SLOT_NONE, false);

		lock_acquire (&swap_lock);
		size_t i = 0;
		while (i < SECTORS_PER_PAGE) {
			disk_read (swap_disk, page->anon.swap_slot * SECTORS_PER_PAGE + i,
			           (uint8_t *) kva + DISK_SECTOR_SIZE * i);
			i++;
		}
		bitmap_set (swap_bitmap, page->anon.swap_slot, false);
		page->anon.swapped = false;
		page->anon.swap_slot = SWAP_SLOT_NONE;
		lock_release (&swap_lock);
	} else {
		memset (kva, 0, PGSIZE);
	}
	return true;
}

// swap out
static bool
anon_swap_out (struct page *page) {
	RETURN_VALUE_IF (page == NULL, false);
	RETURN_VALUE_IF (page->frame == NULL || page->frame->kva == NULL, false);
	RETURN_VALUE_IF (page->anon.swapped, false);
	RETURN_VALUE_IF (swap_disk == NULL || swap_bitmap == NULL, false);

	lock_acquire (&swap_lock);
	size_t slot = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
	if (slot == BITMAP_ERROR) {
		lock_release (&swap_lock);
		return false;
	}
	size_t i = 0;
	while (i < SECTORS_PER_PAGE) {
		disk_write (swap_disk, slot * SECTORS_PER_PAGE + i,
		            (uint8_t *) page->frame->kva + DISK_SECTOR_SIZE * i);
		i++;
	}
	page->anon.swapped = true;
	page->anon.swap_slot = slot;
	lock_release (&swap_lock);

	return true;
}

// destroy
static void
anon_destroy (struct page *page) {
	RETURN_IF (page == NULL);

	if (page->anon.swapped) {
		RETURN_IF (swap_bitmap == NULL || page->anon.swap_slot == SWAP_SLOT_NONE);

		lock_acquire (&swap_lock);
		bitmap_set (swap_bitmap, page->anon.swap_slot, false);
		page->anon.swapped = false;
		page->anon.swap_slot = SWAP_SLOT_NONE;
		lock_release (&swap_lock);
	}
}
