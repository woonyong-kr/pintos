/* vm.c: Generic interface for virtual memory objects. */

#include <string.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "hash.h"
#include "filesys/file.h"
#include "userprog/process.h"
#include "threads/mmu.h"
#define STACK_MAX ((uintptr_t) 1 << 20) /* 유저 스택 최대 크기: 1MB */

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;

/* 각 하위 시스템의 초기화 코드를 호출해 가상 메모리 하위 시스템을 초기화한다. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_table);
	lock_init(&frame_lock);
	clock_hand = NULL;
}

/* 페이지 타입을 얻는다. 아직 초기화되지 않은 페이지가 실제로 어떤 타입으로
 * 초기화될 예정인지 알고 싶을 때 유용하다.
 * 이 함수는 현재 완성되어 있다. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* 보조 함수들 */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static bool vm_frame_evictable (struct frame *frame);
static struct list_elem *vm_next_clock_elem (struct list_elem *e);
static uint64_t page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);
static bool vm_fault_addr_invalid (void *addr);
static bool vm_handle_not_present_fault (struct intr_frame *f, void *addr,
                                         bool user, bool write);
static bool vm_claim_spt_page (void *addr, bool write);
static bool vm_handle_unwritable_spt_page (struct page *page UNUSED);
static bool vm_should_grow_stack (struct intr_frame *f, void *addr, bool user);
static bool vm_grow_stack_and_claim (void *addr);
static bool vm_handle_write_protect_fault (void *addr, bool write);
static bool vm_handle_wp (struct page *page);
static bool vm_handle_cow (struct page *page);
static bool vm_copy_cow_page (struct page *page, struct frame *old_frame);
static bool vm_remap_page (struct page *page, struct frame *frame,
                           bool writable);
static bool spt_copy_cow_page (struct supplemental_page_table *dst,
                               struct page *src_page);
static bool spt_copy_uninit_page (struct page *src_page);
static void spt_page_destroy (struct hash_elem *e, void *aux UNUSED);
static void vm_destroy_page_frame (struct page *page);

static uint64_t
page_hash (const struct hash_elem *e, void *aux UNUSED) {
	struct page *item = hash_entry (e, struct page, hash_elem);
	uint64_t hash_va = hash_bytes (&item->va, sizeof item->va);

	return hash_va;
}

static bool
page_less (const struct hash_elem *a, const struct hash_elem *b,
		void *aux UNUSED) {
	struct page *page_one = hash_entry (a, struct page, hash_elem);
	struct page *page_two = hash_entry (b, struct page, hash_elem);
	return page_one->va < page_two->va;
}


/* 초기화 함수를 가진 대기 상태 페이지 객체를 만든다. 페이지가 필요하면 직접
 * 만들지 말고 이 함수나 vm_alloc_page()를 통해 만들어야 한다. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {
			
	ASSERT(pg_ofs(upage) == 0);		
	ASSERT (VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current ()->spt;
	
	if (upage == NULL || spt_find_page (spt, upage) != NULL) 
		goto err;
		
		struct page* page = malloc(sizeof *page);
		if (page == NULL)
			goto err;
		
		bool (*initializer)(struct page *page, enum vm_type type, void *kva) = NULL;

		switch (VM_TYPE(type)) {
			case VM_ANON:
				initializer = anon_initializer;
				break;
			
			case VM_FILE:
				initializer = file_backed_initializer;
				break;
			
			default:
				free(page);
				goto err;
		}
		
		uninit_new(page, upage, init, type, aux, initializer);
		page->writable = writable;

		if(!spt_insert_page(spt, page)) {
			free(page);
			goto err;
		}
		else 
			return true;
		
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/* 페이지 폴트가 발생하면 spt 에서 va로 메타 데이터를 찾는 함수 */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	
	RETURN_VALUE_IF (spt == NULL || va == NULL, NULL);
	
	void * rounded_va = pg_round_down(va); 
	struct page temp;
	temp.va = rounded_va; 
	
	struct hash_elem * found = hash_find(&spt->hash_table, &temp.hash_elem);
	RETURN_VALUE_IF (found == NULL, NULL);

	struct page *found_page = hash_entry(found, struct page, hash_elem);
	return found_page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {

	RETURN_VALUE_IF (spt == NULL || page == NULL, false);
	ASSERT(pg_ofs(page->va) == 0);		
	
	return hash_insert(&spt->hash_table, &page->hash_elem) == NULL;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	RETURN_IF (spt == NULL || page == NULL);

	hash_delete (&spt->hash_table, &page->hash_elem);
	vm_dealloc_page (page);

	// return true;
}

/* 교체 대상이 될 struct frame을 고른다. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	struct list_elem *e;
	size_t scan_count;
	size_t max_scans;

	lock_acquire (&frame_lock);
	if (list_empty (&frame_table))
		goto done;

	if (clock_hand == NULL || clock_hand == list_end (&frame_table))
		clock_hand = list_begin (&frame_table);

	max_scans = list_size (&frame_table) * 2;
	for (scan_count = 0; scan_count < max_scans; scan_count++) {
		e = clock_hand;
		struct frame *frame = list_entry (e, struct frame, elem);
		struct page *page;
		struct thread *owner;

		if (!vm_frame_evictable (frame))
			goto next;

		page = frame->page;
		owner = frame->owner_thread;
		if (owner != NULL && owner->pml4 != NULL &&
		    pml4_is_accessed (owner->pml4, page->va)) {
			pml4_set_accessed (owner->pml4, page->va, false);
			goto next;
		}

		victim = frame;
		clock_hand = vm_next_clock_elem (clock_hand);
		break;
next:
		clock_hand = vm_next_clock_elem (clock_hand);
	}
done:
	lock_release (&frame_lock);

	return victim;
}

static bool
vm_frame_evictable (struct frame *frame) {
	return frame != NULL && frame->kva != NULL && frame->page != NULL &&
	       frame->ref_count == 1;
}

static struct list_elem *
vm_next_clock_elem (struct list_elem *e) {
	if (e == NULL || e == list_end (&frame_table))
		return list_begin (&frame_table);
	e = list_next (e);
	return e == list_end (&frame_table) ? list_begin (&frame_table) : e;
}

/* 페이지 하나를 내보내고 그 페이지에 대응하던 프레임을 반환한다.
 * 오류가 있으면 NULL을 반환한다. */
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	struct page *page;
	struct thread *owner;

	RETURN_VALUE_IF (victim == NULL || victim->page == NULL, NULL);

	page = victim->page;
	owner = victim->owner_thread;
	RETURN_VALUE_IF (!swap_out (page), NULL);

	if (owner != NULL && owner->pml4 != NULL)
		pml4_clear_page (owner->pml4, page->va);

	page->frame = NULL;
	victim->page = NULL;
	victim->owner_thread = NULL;
	victim->ref_count = 1;
	return victim;
}

/* palloc()을 호출해서 프레임을 얻는다.
 * 사용 가능한 페이지가 없으면, 기존 페이지 하나를 eviction해서
 * 그 프레임을 반환한다.
 *
 * 이 함수는 항상 유효한 주소를 반환한다.
 * 즉, user pool 메모리가 가득 차 있더라도 페이지를 eviction해서
 * 사용할 수 있는 메모리 공간을 확보한다.
 */
/* user pool 에서 공간 할당 받아오기
frame에 올리기 위해 할당받는곳 
 */
static struct frame *
vm_get_frame (void) {
	struct frame *frame = malloc(sizeof* frame);
	RETURN_VALUE_IF (frame == NULL, NULL);

	frame->kva = palloc_get_page (PAL_USER);
	if(frame->kva != NULL) {
		frame->page = NULL;
	}
	else {
		free(frame);
		frame = vm_evict_frame ();
		RETURN_VALUE_IF (frame == NULL, NULL);
	}
	/*
	list_init(&frame_table) vm 초기화 시점에 있어야함

	lock_acquire(&frame_table_lock)
	list_push_back(...)
	lock_release(&frame_table_lock)
	 */
	frame->owner_thread = thread_current ();
	frame->ref_count = 1;

	if (frame->page == NULL) {
		bool in_table = false;
		struct list_elem *e;

		lock_acquire (&frame_lock);
		for (e = list_begin (&frame_table); e != list_end (&frame_table);
		     e = list_next (e)) {
			if (list_entry (e, struct frame, elem) == frame) {
				in_table = true;
				break;
			}
		}
		if (!in_table)
			list_push_back(&frame_table, &frame->elem);
		lock_release (&frame_lock);
	}

	// ASSERT (frame != NULL);
	// ASSERT (frame->page == NULL);

	// 만든 frame free는 언제하지 
	return frame;
}

/* 스택을 확장한다. */
static void
vm_stack_growth (void *addr UNUSED) {
	struct thread *curr = thread_current ();

	RETURN_IF (addr == NULL || curr == NULL || curr->stack_bottom == NULL);

	void *stack_bottom = curr->stack_bottom;
	void *page = pg_round_down (addr);

	while (page < stack_bottom) {
		stack_bottom -= PGSIZE;

		RETURN_IF (!vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true));
		RETURN_IF (!vm_claim_page (stack_bottom));

		curr->stack_bottom = stack_bottom;
	}
}

/* 쓰기 보호 페이지에서 발생한 폴트를 처리한다. */
static bool
vm_handle_wp (struct page *page) {
	struct thread *curr = thread_current ();

	RETURN_VALUE_IF (page == NULL || curr == NULL || curr->pml4 == NULL, false);
	RETURN_VALUE_IF (!page->writable, false);
	RETURN_VALUE_IF (page->frame == NULL || page->frame->kva == NULL, false);

	return vm_handle_cow (page);
}

static bool
vm_handle_cow (struct page *page) {
	struct frame *old_frame;

	RETURN_VALUE_IF (page == NULL, false);

	old_frame = page->frame;
	RETURN_VALUE_IF (old_frame == NULL || old_frame->kva == NULL, false);

	if (old_frame->ref_count > 1)
		return vm_copy_cow_page (page, old_frame);

	return vm_remap_page (page, old_frame, true);
}

static bool
vm_copy_cow_page (struct page *page, struct frame *old_frame) {
	struct frame *new_frame;

	RETURN_VALUE_IF (page == NULL || old_frame == NULL, false);
	RETURN_VALUE_IF (old_frame->kva == NULL, false);

	new_frame = vm_get_frame ();
	RETURN_VALUE_IF (new_frame == NULL, false);

	memcpy (new_frame->kva, old_frame->kva, PGSIZE);

	lock_acquire (&frame_lock);
	old_frame->ref_count--;
	lock_release (&frame_lock);

	return vm_remap_page (page, new_frame, true);
}

static bool
vm_remap_page (struct page *page, struct frame *frame, bool writable) {
	struct thread *curr = thread_current ();

	RETURN_VALUE_IF (page == NULL || frame == NULL, false);
	RETURN_VALUE_IF (curr == NULL || curr->pml4 == NULL, false);
	RETURN_VALUE_IF (page->va == NULL || frame->kva == NULL, false);

	pml4_clear_page (curr->pml4, page->va);
	RETURN_VALUE_IF (!pml4_set_page (curr->pml4, page->va,
	                                 frame->kva, writable), false);

	frame->page = page;
	frame->owner_thread = curr;
	page->frame = frame;
	return true;
}


// OS가 복구할 수 있는 page fault를 처리
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
                     bool user, bool write, bool not_present) {
	RETURN_VALUE_IF (vm_fault_addr_invalid (addr), false);
	RETURN_VALUE_IF (!not_present, vm_handle_write_protect_fault (addr, write));
	return vm_handle_not_present_fault (f, addr, user, write);
}
// fault 주소가 NULL이거나 커널 영역이면 복구할 수 없는 fault로 판단
static bool
vm_fault_addr_invalid (void *addr) {
	return addr == NULL || is_kernel_vaddr (addr);
}

/* 페이지가 아직 매핑되지 않은 fault를 처리한다. */
static bool
vm_handle_not_present_fault (struct intr_frame *f, void *addr,
                             bool user, bool write) {
	RETURN_VALUE_IF (vm_claim_spt_page (addr, write), true);
	RETURN_VALUE_IF (!vm_should_grow_stack (f, addr, user), false);
	return vm_grow_stack_and_claim (addr);
}

// SPT에 등록된 page가 있으면 실제 프레임 매핑
static bool
vm_claim_spt_page (void *addr, bool write) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = spt_find_page (spt, addr);

	// SPT에 page가 없거나, write 요청이 있고 page가 writable이 아니면 실패
	RETURN_VALUE_IF (page == NULL, false);
	RETURN_VALUE_IF (write && !page->writable, vm_handle_unwritable_spt_page (page));

	// page 맵핑
	return vm_do_claim_page (page);
}

// SPT page는 찾았지만 쓰기 권한을 허용할 수 없는 fault 처리
static bool
vm_handle_unwritable_spt_page (struct page *page UNUSED) {
	/* TODO: copy-on-write를 구현하면 여기서 처리한다. */
	return false;
}

// addr가 정상적인 스택 성장인지 판단
static bool
vm_should_grow_stack (struct intr_frame *f, void *addr, bool user) {
	RETURN_VALUE_IF (addr == NULL, false);
	RETURN_VALUE_IF (user && f == NULL, false);

	struct thread *curr = thread_current ();
	RETURN_VALUE_IF (!user && curr == NULL, false);

	uintptr_t fault_addr = (uintptr_t) addr;
	uintptr_t rsp = user ? (uintptr_t) f->rsp : (uintptr_t) curr->user_rsp;

	RETURN_VALUE_IF (rsp < 32, false);

	return fault_addr < (uintptr_t) USER_STACK &&
	       fault_addr >= (uintptr_t) (USER_STACK - STACK_MAX) &&
	       fault_addr >= rsp - 32;
}

/* 스택 페이지를 만들고 즉시 claim한다. */
static bool
vm_grow_stack_and_claim (void *addr) {
	/* TODO: vm_stack_growth()에서 pg_round_down(addr)에 VM_ANON 스택
	 * 페이지를 SPT에 추가하게 만든 뒤 claim한다. */
	vm_stack_growth (addr);
	return vm_claim_page (pg_round_down (addr));
}

/* 이미 존재하는 페이지의 권한 위반 fault를 처리한다. */
static bool
vm_handle_write_protect_fault (void *addr, bool write) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = spt_find_page (spt, addr);
	RETURN_VALUE_IF (!write, false);
	RETURN_VALUE_IF (page == NULL, false);

	return vm_handle_wp (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	vm_destroy_page_frame (page);
	free (page);
}

/* Claim the page that allocate on VA. */
/* 인자로 받은 va에 해당하는 page를 SPT에서 찾고, 그 page를 실제 frame에 올리는 함수 */
bool
vm_claim_page (void *va) {
	struct thread *curr = thread_current ();
	RETURN_VALUE_IF (va == NULL || curr == NULL, false);

	struct supplemental_page_table *spt = &curr->spt;
	struct page* page = spt_find_page(spt, va);
	RETURN_VALUE_IF (page == NULL, false);

	return vm_do_claim_page (page);
	
}

/* PAGE를 실제 메모리에 올리고 MMU 매핑을 설정한다. */
static bool
vm_do_claim_page (struct page *page) {
	struct thread *curr = thread_current ();
	RETURN_VALUE_IF (page == NULL || curr == NULL, false);

	struct frame *frame = vm_get_frame ();
	RETURN_VALUE_IF(frame == NULL , false);

	frame->page = page;
	frame->owner_thread = curr;
	page->frame = frame;

	if (!pml4_set_page (curr->pml4, page->va, frame->kva, page->writable)) {
		frame->page = NULL;
		page->frame = NULL;
		lock_acquire (&frame_lock);
		list_remove (&frame->elem);
		lock_release (&frame_lock);
		palloc_free_page (frame->kva);
		free (frame);
		return false;
	}

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	ASSERT(hash_init(&spt->hash_table, page_hash, page_less, NULL));
	ASSERT(hash_empty(&spt->hash_table));
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
                              struct supplemental_page_table *src UNUSED) {

	struct hash_iterator i;

	RETURN_VALUE_IF (dst == NULL || src == NULL, false);

	hash_first (&i, &src->hash_table);
	while (hash_next (&i)) {
		struct page *src_page = hash_entry (hash_cur (&i), struct page,
		                                    hash_elem);
		enum vm_type page_type = page_get_type (src_page);

		if (page_type == VM_FILE)
			continue;

		if (page_type == VM_UNINIT) {
			if (!spt_copy_uninit_page (src_page))
				return false;
			continue;
		}

		if (page_type == VM_ANON && src_page->frame != NULL) {
			if (!spt_copy_cow_page (dst, src_page))
				return false;
			continue;
		}

		if (!vm_alloc_page (page_type, src_page->va, src_page->writable))
			return false;

		struct page *child_page = spt_find_page (dst, src_page->va);
		if (child_page == NULL)
			return false;
		if (!vm_do_claim_page (child_page))
			return false;
		if (src_page->frame != NULL)
			memcpy (child_page->frame->kva, src_page->frame->kva, PGSIZE);
	}
	return true;
}

static bool
spt_copy_uninit_page (struct page *src_page) {
	void *aux;

	RETURN_VALUE_IF (src_page == NULL, false);

	aux = src_page->uninit.aux;
	if (aux != NULL) {
		struct lazy_load_arg *temp_aux = malloc (sizeof *temp_aux);
		RETURN_VALUE_IF (temp_aux == NULL, false);

		memcpy (temp_aux, aux, sizeof *temp_aux);
		if (temp_aux->file != NULL) {
			temp_aux->file = file_reopen (temp_aux->file);
			if (temp_aux->file == NULL) {
				free (temp_aux);
				return false;
			}
		}
		aux = temp_aux;
	}

	if (!vm_alloc_page_with_initializer (src_page->uninit.type, src_page->va,
	                                     src_page->writable,
	                                     src_page->uninit.init,
	                                     aux)) {
		struct lazy_load_arg *temp_aux = aux;

		if (temp_aux != src_page->uninit.aux && temp_aux != NULL) {
			if (temp_aux->file != NULL)
				file_close (temp_aux->file);
			free (temp_aux);
		}
		return false;
	}
	return true;
}

static bool
spt_copy_cow_page (struct supplemental_page_table *dst,
                   struct page *src_page) {
	struct page *dst_page;
	struct frame *frame;
	struct thread *owner;
	struct thread *curr;

	RETURN_VALUE_IF (dst == NULL || src_page == NULL, false);
	RETURN_VALUE_IF (!vm_alloc_page (page_get_type (src_page), src_page->va,
	                                 src_page->writable), false);

	dst_page = spt_find_page (dst, src_page->va);
	RETURN_VALUE_IF (dst_page == NULL, false);

	frame = src_page->frame;
	if (frame == NULL || frame->kva == NULL)
		return true;

	owner = frame->owner_thread;
	curr = thread_current ();
	if (owner == NULL || owner->pml4 == NULL || curr == NULL ||
	    curr->pml4 == NULL) {
		spt_remove_page (dst, dst_page);
		return false;
	}

	if (src_page->writable &&
	    !pml4_set_page (owner->pml4, src_page->va, frame->kva, false)) {
		spt_remove_page (dst, dst_page);
		return false;
	}

	if (!pml4_set_page (curr->pml4, dst_page->va, frame->kva, false)) {
		if (src_page->writable)
			pml4_set_page (owner->pml4, src_page->va, frame->kva,
			               src_page->writable);
		spt_remove_page (dst, dst_page);
		return false;
	}

	dst_page->operations = src_page->operations;
	dst_page->anon = src_page->anon;
	dst_page->frame = frame;

	lock_acquire (&frame_lock);
	frame->ref_count++;
	lock_release (&frame_lock);
	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	RETURN_IF (spt == NULL);
	hash_destroy (&spt->hash_table, spt_page_destroy);
}

static void
spt_page_destroy (struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry (e, struct page, hash_elem);
	vm_dealloc_page (page);
}

static void
vm_destroy_page_frame (struct page *page) {
	struct frame *frame;
	struct thread *curr;
	bool release_frame = false;

	RETURN_IF (page == NULL || page->frame == NULL);

	frame = page->frame;
	curr = thread_current ();

	if (curr != NULL && curr->pml4 != NULL)
		pml4_clear_page (curr->pml4, page->va);

	page->frame = NULL;

	lock_acquire (&frame_lock);
	if (frame->ref_count > 1) {
		frame->ref_count--;
	} else {
		list_remove (&frame->elem);
		release_frame = true;
	}
	lock_release (&frame_lock);

	if (!release_frame)
		return;

	if (frame->kva != NULL)
		palloc_free_page (frame->kva);
	frame->page = NULL;
	frame->owner_thread = NULL;
	free (frame);
}
