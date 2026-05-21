/* file.c: Implementation of memory backed file object (mmaped object). */

#include <string.h>
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "vm/vm.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool lazy_load_file (struct page *page, void *aux);
static bool mmap_page_matches (struct page *page, void *map_start);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	struct lazy_load_arg *aux;
	struct file_page *file_page;

	/* Set up the handler */
	RETURN_VALUE_IF (page == NULL, false);
	aux = page->uninit.aux;
	RETURN_VALUE_IF (aux == NULL || aux->file == NULL, false);

	page->operations = &file_ops;

	file_page = &page->file;
	file_page->file = aux->file;
	file_page->ofs = aux->ofs;
	file_page->page_read_bytes = aux->page_read_bytes;
	file_page->page_zero_bytes = aux->page_zero_bytes;
	file_page->map_start = aux->map_start;

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page;
	off_t bytes_read = 0;

	RETURN_VALUE_IF (page == NULL || kva == NULL, false);
	file_page = &page->file;
	RETURN_VALUE_IF (file_page->file == NULL, false);

	if (file_page->page_read_bytes > 0) {
		lock_acquire (&filesys_lock);
		bytes_read = file_read_at (file_page->file, kva,
		                           file_page->page_read_bytes,
		                           file_page->ofs);
		lock_release (&filesys_lock);
		if (bytes_read != (off_t) file_page->page_read_bytes)
			return false;
	}
	memset ((uint8_t *) kva + file_page->page_read_bytes, 0,
	        file_page->page_zero_bytes);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct thread *owner;
	struct file_page *file_page;

	RETURN_VALUE_IF (page == NULL, false);
	RETURN_VALUE_IF (page->frame == NULL || page->frame->kva == NULL, true);
	owner = page->frame->owner_thread != NULL ? page->frame->owner_thread :
	        thread_current ();
	RETURN_VALUE_IF (owner == NULL || owner->pml4 == NULL, false);

	file_page = &page->file;
	RETURN_VALUE_IF (file_page->file == NULL, false);

	if (file_page->page_read_bytes > 0 &&
	    pml4_is_dirty (owner->pml4, page->va)) {
		off_t written;

		lock_acquire (&filesys_lock);
		written = file_write_at (file_page->file, page->frame->kva,
		                         file_page->page_read_bytes,
		                         file_page->ofs);
		lock_release (&filesys_lock);
		if (written != (off_t) file_page->page_read_bytes)
			return false;
		pml4_set_dirty (owner->pml4, page->va, false);
	}
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page;

	RETURN_IF (page == NULL);
	file_page = &page->file;

	file_backed_swap_out (page);
	if (file_page->file != NULL) {
		lock_acquire (&filesys_lock);
		file_close (file_page->file);
		lock_release (&filesys_lock);
		file_page->file = NULL;
	}
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	void *start_addr = addr;
	size_t checked = 0;
	size_t remaining = length;
	off_t file_len;
	off_t file_remaining;

	RETURN_VALUE_IF (addr == NULL || file == NULL || length == 0, NULL);

	lock_acquire (&filesys_lock);
	file_len = file_length (file);
	lock_release (&filesys_lock);
	RETURN_VALUE_IF (file_len == 0, NULL);

	file_remaining = offset < file_len ? file_len - offset : 0;
	while (checked < length) {
		void *upage = (uint8_t *) addr + checked;

		RETURN_VALUE_IF (spt_find_page (spt, upage) != NULL, NULL);
		checked += PGSIZE;
	}

	while (remaining > 0) {
		struct lazy_load_arg *aux = malloc (sizeof *aux);
		size_t page_read_bytes;

		if (aux == NULL)
			goto fail;

		page_read_bytes = remaining < PGSIZE ? remaining : PGSIZE;
		if ((off_t) page_read_bytes > file_remaining)
			page_read_bytes = file_remaining > 0 ? file_remaining : 0;

		aux->file = file_reopen (file);
		if (aux->file == NULL) {
			free (aux);
			goto fail;
		}
		aux->ofs = offset;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = PGSIZE - page_read_bytes;
		aux->map_start = start_addr;

		if (!vm_alloc_page_with_initializer (VM_FILE, addr, writable,
		                                     lazy_load_file, aux)) {
			lock_acquire (&filesys_lock);
			file_close (aux->file);
			lock_release (&filesys_lock);
			free (aux);
			goto fail;
		}

		remaining -= remaining < PGSIZE ? remaining : PGSIZE;
		offset += page_read_bytes;
		file_remaining -= page_read_bytes;
		addr = (uint8_t *) addr + PGSIZE;
	}
	return start_addr;

fail:
	do_munmap (start_addr);
	return NULL;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	void *upage = addr;

	RETURN_IF (addr == NULL || pg_ofs (addr) != 0);

	for (;;) {
		struct page *page = spt_find_page (spt, upage);

		if (!mmap_page_matches (page, addr))
			break;
		spt_remove_page (spt, page);
		upage = (uint8_t *) upage + PGSIZE;
	}
}

static bool
lazy_load_file (struct page *page, void *aux_) {
	struct lazy_load_arg *aux = aux_;
	bool success = file_backed_swap_in (page, page->frame->kva);

	free (aux);
	return success;
}

static bool
mmap_page_matches (struct page *page, void *map_start) {
	RETURN_VALUE_IF (page == NULL || page_get_type (page) != VM_FILE, false);

	if (page->operations->type == VM_UNINIT) {
		struct lazy_load_arg *aux = page->uninit.aux;

		return aux != NULL && aux->map_start == map_start;
	}
	return page->file.map_start == map_start;
}
