/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "string.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool lazy_load_file (struct page *page, void *aux_);
static struct lock file_lock;

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
	lock_init(&file_lock);
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	struct file_page *file_page = &page->file;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	if (file_read_at(file_page->file, kva, file_page->page_read_bytes, file_page->ofs) != file_page->page_read_bytes)
		return false;
	memset(kva + file_page->page_read_bytes, 0, PGSIZE - file_page->page_read_bytes);
	file_page->swapped = false;
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	
	if (pml4_is_dirty(page->frame->owner_thread->pml4, page->va)) {
		file_write_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->ofs);
	}
	pml4_clear_page(page->frame->owner_thread->pml4, page->va);
	file_page->swapped = true;
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	if (page->frame != NULL){
		if (pml4_is_dirty(page->frame->owner_thread->pml4, page->va)) {
			file_write_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->ofs);
		}
	}
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	void *start_addr = addr;
	struct file *reopened_file = file_reopen(file);
	off_t file_size = file_length(reopened_file);

	size_t f_read_bytes = length < file_size - offset ? length : file_size - offset;
	while (f_read_bytes > 0){
		struct lazy_load_file_arg* aux = malloc(sizeof(struct lazy_load_file_arg));
		RETURN_VALUE_IF (aux == NULL, false);
	
		size_t page_read_bytes = f_read_bytes < PGSIZE ? f_read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		aux->file = reopened_file;
		if (aux->file == NULL) {
			free (aux);
			return false;
		}
		aux->ofs = offset;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = page_zero_bytes;
		
		if (!vm_alloc_page_with_initializer (VM_FILE, addr, writable,
		                                     lazy_load_file, aux)) {
				free(aux);
				return false;																		
			}	
		/*
		 * 다음 페이지로 진행한다.
		 */
		addr += PGSIZE;
		offset += page_read_bytes;
		f_read_bytes -= page_read_bytes;
	}
	
	return start_addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct page* page;
	struct file* page_file = spt_find_page(&thread_current()->spt, addr)->file.file;

	while (page = spt_find_page(&thread_current()->spt, addr)){
		if (page_get_type(page) == VM_FILE && page->file.file == page_file){
			spt_remove_page(&thread_current()->spt, page);
			addr += PGSIZE;
		}
		else{
			break;
		}
	}
	file_close(page_file);
}

static bool
lazy_load_file (struct page *page, void *aux_) {
	struct lazy_load_file_arg *aux = aux_;
	uint8_t *kva = page->frame->kva;
	bool success = false;

	if (aux == NULL || aux->file == NULL)
		goto done;

	lock_acquire (&file_lock);
	if (file_read_at (aux->file, kva, aux->page_read_bytes, aux->ofs) == (off_t) aux->page_read_bytes) {
		memset (kva + aux->page_read_bytes, 0, aux->page_zero_bytes);
		success = true;
	}
	lock_release (&file_lock);

	if(success){
		page->file.file = aux->file;
		page->file.ofs = aux->ofs;
		page->file.page_read_bytes = aux->page_read_bytes;
		page->file.swapped = false;
	}

	done:
	if (aux != NULL) {
		free (aux);
	}
	return success;
}


