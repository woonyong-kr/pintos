/* file.c: 메모리에 매핑된 파일 객체, 즉 mmap 객체 구현. */

#include <stdint.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "string.h"

struct mmap_lazy_arg {
	struct file *file;
	off_t ofs;
	size_t read_bytes;
	size_t zero_bytes;
	void *map_start;
};

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool file_copy (struct supplemental_page_table *dst, struct page *src_page);
static bool lazy_load_file (struct page *page, void *aux_);
static struct lock file_lock;

/* 이 구조체는 수정하지 말 것. */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.copy = file_copy,
	.type = VM_FILE,
};

/* 파일 기반 VM을 초기화한다. */
void
vm_file_init (void) {
	lock_init(&file_lock);
}

/* 파일 기반 페이지를 초기화한다. */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva UNUSED) {
	struct file_page *file_page;

	RETURN_VALUE_IF (page == NULL, false);
	page->operations = &file_ops;
	struct file_page *file_page = &page->file;
	return true;
}

/* 파일에서 내용을 읽어 페이지를 swap in한다. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	if (file_read_at(file_page->file, kva, file_page->page_read_bytes, file_page->ofs) != file_page->page_read_bytes)
		return false;
	memset(kva + file_page->page_read_bytes, 0, PGSIZE - file_page->page_read_bytes);
	file_page->swapped = false;
	return true;
}

/* 내용을 파일에 쓰기 반영해 페이지를 swap out한다. */
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

/* 파일 기반 페이지를 파괴한다. PAGE 자체는 호출자가 해제한다. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	if (page->frame != NULL){
		if (pml4_is_dirty(page->frame->owner_thread->pml4, page->va)) {
			file_write_at(file_page->file, page->frame->kva, file_page->page_read_bytes, file_page->ofs);
		}
	}
}

// mmap
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

/* munmap을 수행한다. */
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

static bool
file_copy (struct supplemental_page_table *dst, struct page *src_page){
	RETURN_FALSE_IF (src_page == NULL);
	
	RETURN_FALSE_IF(!vm_alloc_page (page_get_type(src_page), src_page->va, src_page->writable));
	RETURN_FALSE_IF(!vm_claim_page (src_page->va));

	struct page *dst_page = spt_find_page (dst, src_page->va);
	if (src_page->file.swapped){
		RETURN_FALSE_IF (file_read_at(src_page->file.file, src_page->frame->kva, src_page->file.page_read_bytes, src_page->file.ofs) != src_page->file.page_read_bytes);
	}else{
		memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
	}
	return true;
}
