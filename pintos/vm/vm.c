/* vm.c: 가상 메모리 객체의 공통 인터페이스. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"

#define STACK_MAX ((uintptr_t) 1 << 20) /* 유저 스택 최대 크기: 1MB */

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem* clock_ptr;

/* 각 하위 시스템의 초기화 코드를 호출해 가상 메모리 하위 시스템을 초기화한다. */
void
vm_init (void) {
	list_init (&frame_table);
	lock_init (&frame_table_lock);
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS /* 프로젝트 4용 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_table);
	lock_init(&frame_lock);
	clock_ptr = NULL;
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
	case VM_ANON:
		return VM_TYPE (page->anon.type);
	default:
		return ty;
	}
}

/* 보조 함수들 */
static struct spt_entry *spt_entry_from_page (struct page *page);
static uint64_t spt_entry_hash (const struct hash_elem *e, void *aux);
static bool spt_entry_less (const struct hash_elem *a,
                            const struct hash_elem *b, void *aux);
static void spt_entry_destroy (struct hash_elem *e, void *aux);
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static bool vm_fault_addr_invalid (void *addr);
static bool vm_handle_not_present_fault (struct intr_frame *f, void *addr,
                                         bool user, bool write);
static bool vm_claim_spt_page (void *addr, bool write);
static bool vm_handle_unwritable_spt_page (struct page *page);
static bool vm_should_grow_stack (struct intr_frame *f, void *addr, bool user);
static bool vm_grow_stack_and_claim (void *addr);
static bool vm_handle_write_protect_fault (void *addr, bool write);
static bool vm_handle_wp (struct page *page);
static bool vm_handle_cow (struct page *page) UNUSED;
static bool vm_copy_cow_page (struct page *page, struct frame *old_frame) UNUSED;
static bool vm_remap_page (struct page *page, struct frame *frame,
                           bool writable) UNUSED;
static bool spt_copy_page (struct supplemental_page_table *dst,
                           struct page *page);
static bool spt_copy_uninit_page (struct page *page);
static bool spt_copy_anon_page (struct supplemental_page_table *dst,
                                struct page *page);
static bool spt_copy_lazy_aux (struct page *page, void **aux);
static void spt_free_lazy_aux (void *aux);
static void vm_destroy_page_frame (struct page *page);

static struct spt_entry *
spt_entry_from_page (struct page *page) {
	return (struct spt_entry *) ((char *) page - offsetof (struct spt_entry, page));
}

static void
vm_destroy_page_frame (struct page *page);
static void
spt_page_destroy (struct hash_elem *e, void *aux UNUSED);

static uint64_t
spt_entry_hash (const struct hash_elem *e, void *aux UNUSED) {
	struct spt_entry *entry = hash_entry (e, struct spt_entry, hash_elem);
	return hash_bytes (&entry->page.va, sizeof entry->page.va);
}

static bool
spt_entry_less (const struct hash_elem *a,
                const struct hash_elem *b, void *aux UNUSED) {
	struct spt_entry *left_entry = hash_entry (a, struct spt_entry, hash_elem);
	struct spt_entry *right_entry = hash_entry (b, struct spt_entry, hash_elem);
	return (uintptr_t) left_entry->page.va < (uintptr_t) right_entry->page.va;
}

static void
spt_entry_destroy (struct hash_elem *e, void *aux UNUSED) {
	struct spt_entry *entry = hash_entry (e, struct spt_entry, hash_elem);
	vm_destroy_page_frame (&entry->page);
	destroy (&entry->page);
	free (entry);
}

/* 초기화 함수를 가진 대기 상태 페이지 객체를 만든다. 페이지가 필요하면 직접
 * 만들지 말고 이 함수나 vm_alloc_page()를 통해 만들어야 한다. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
                                vm_initializer *init, void *aux) {
	ASSERT (VM_TYPE (type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;
	enum vm_type page_type = VM_TYPE (type);
	struct spt_entry *entry;
	bool (*page_initializer) (struct page *, enum vm_type, void *) = NULL;

	switch (page_type) {
	case VM_ANON:
		page_initializer = anon_initializer;
		break;
	case VM_FILE:
		page_initializer = file_backed_initializer;
		break;
	default:
		return false;
	}

	RETURN_VALUE_IF (upage == NULL || spt_find_page (spt, upage) != NULL, false);

	entry = malloc (sizeof *entry);
	RETURN_VALUE_IF (entry == NULL, false);

	uninit_new (&entry->page, pg_round_down (upage), init, type, aux,
	            page_initializer);
	entry->page.writable = writable;

	if (!spt_insert_page (spt, &entry->page)) {
		vm_dealloc_page (&entry->page);
		return false;
	}

	return true;
}

/* SPT에서 VA에 해당하는 페이지를 찾아 반환한다. 오류가 있으면 NULL을 반환한다. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct spt_entry probe;
	struct hash_elem *found;

	RETURN_VALUE_IF (spt == NULL || va == NULL, NULL);

	probe.page.va = pg_round_down (va);
	found = hash_find (&spt->pages, &probe.hash_elem);

	RETURN_VALUE_IF (found == NULL, NULL);

	return &hash_entry (found, struct spt_entry, hash_elem)->page;
}
/* PAGE를 검증한 뒤 SPT에 삽입한다. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	RETURN_VALUE_IF (spt == NULL || page == NULL, false);

	page->va = pg_round_down (page->va);
	return hash_insert (&spt->pages, &spt_entry_from_page (page)->hash_elem) == NULL;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	RETURN_IF (spt == NULL || page == NULL);

	hash_delete (&spt->pages, &spt_entry_from_page (page)->hash_elem);
	vm_dealloc_page (page);
}

// 교체 대상이 될 frame 뽑기
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	/* TODO: 교체 정책은 직접 정한다. */
	//clock algorithm
	if (clock_ptr == NULL)
		clock_ptr = list_begin(&frame_table);

	while (true){
		if (clock_ptr == list_end(&frame_table)) {
            clock_ptr = list_begin(&frame_table);
            continue;
        }

        struct frame *clock_frame = list_entry(clock_ptr, struct frame, elem);
        bool accessed = pml4_is_accessed(clock_frame->owner_thread->pml4, clock_frame->page->va);

        if (!accessed) {
            clock_ptr = list_next(clock_ptr);
            return clock_frame;
        }

        pml4_set_accessed(clock_frame->owner_thread->pml4, clock_frame->page->va, false);
        
        clock_ptr = list_next(clock_ptr);
    }

	return victim;
}

/* 페이지 하나를 내보내고 그 페이지에 대응하던 프레임을 반환한다.
 * 오류가 있으면 NULL을 반환한다. */
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	if (!swap_out(victim->page))
		return NULL;
	victim->owner_thread = NULL;
	victim->page->frame = NULL;
	victim->page = NULL;
	return victim;
}

/* palloc()으로 프레임을 얻는다. 사용 가능한 페이지가 없으면 페이지를 내보낸 뒤
 * 그 프레임을 반환한다. 이 함수는 항상 유효한 주소를 반환해야 한다. 즉 사용자
 * 풀 메모리가 가득 차면 프레임을 교체해 사용 가능한 메모리 공간을 확보한다. */
static struct frame *
vm_get_frame (void) {
	void* kva = palloc_get_page (PAL_USER);

	if(kva == NULL) 
		return vm_evict_frame ();

	struct frame *frame = malloc(sizeof* frame);
	if (frame == NULL){
		palloc_free_page(kva);
		return NULL;
	}
	
	frame->kva = kva;
	frame->page = NULL;
	frame->owner_thread = thread_current();

	lock_acquire(&frame_lock);
	list_push_back(&frame_table, &frame->elem);
	lock_release(&frame_lock);

	return frame;
}

// 스택 확장
static void
vm_stack_growth (void *addr UNUSED) {
	struct thread *curr = thread_current ();

	RETURN_IF (addr == NULL || curr == NULL || curr->stack_bottom == NULL);

	void *stack_bottom = curr->stack_bottom;
	void *page = pg_round_down (addr);

	while (page < stack_bottom) {
		stack_bottom -= PGSIZE;

		RETURN_IF (!vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true));
		// RETURN_IF (!vm_claim_page (stack_bottom)); claim 2번 들어감

		curr->stack_bottom = stack_bottom;
	}
}

// 쓰기 보호 페이지 폴트
static bool
vm_handle_wp (struct page *page) {
	struct thread *curr = thread_current ();

	// 논리적으로 쓰기 가능한 페이지가 실제 PTE에서는 read-only인 경우,
	// COW 페이지일 수 있으므로 전용 처리로 넘김
	RETURN_VALUE_IF (page == NULL || curr == NULL || curr->pml4 == NULL, false);
	RETURN_VALUE_IF (!page->writable, false);
	RETURN_VALUE_IF (page->frame == NULL || page->frame->kva == NULL, false);

	return vm_handle_cow (page);
}

static bool
vm_handle_cow (struct page *page UNUSED) {
	RETURN_VALUE_IF (page == NULL, false);

	struct frame *old_frame = page->frame;

	RETURN_VALUE_IF (old_frame == NULL || old_frame->kva == NULL, false);
	RETURN_VALUE_IF (old_frame->ref_count > 1, vm_copy_cow_page (page, old_frame));

	return vm_remap_page (page, old_frame, true);
}

static bool
vm_copy_cow_page (struct page *page UNUSED, struct frame *old_frame UNUSED) {
	RETURN_VALUE_IF (page == NULL || old_frame == NULL, false);
	RETURN_VALUE_IF (old_frame->kva == NULL, false);

	struct frame *new_frame = vm_get_frame ();
	RETURN_VALUE_IF (new_frame == NULL, false);

	memcpy (new_frame->kva, old_frame->kva, PGSIZE);

	old_frame->ref_count--;

	return vm_remap_page (page, new_frame, true);
}

static bool
vm_remap_page (struct page *page UNUSED, struct frame *frame UNUSED,
               bool writable UNUSED) {
	struct thread *curr = thread_current ();

	RETURN_VALUE_IF (page == NULL || frame == NULL, false);
	RETURN_VALUE_IF (curr == NULL || curr->pml4 == NULL, false);
	RETURN_VALUE_IF (page->va == NULL || frame->kva == NULL, false);

	pml4_clear_page (curr->pml4, page->va);
	RETURN_VALUE_IF (!pml4_set_page (curr->pml4, page->va, frame->kva, writable), false);

	frame->page = page;
	frame->owner = curr;
	page->frame = frame;
	return true;
}

// OS가 복구할 수 있는 page fault를 처리
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
                     bool user, bool write, bool not_present) {
	RETURN_VALUE_IF (vm_fault_addr_invalid (addr), false);
	return !not_present ? vm_handle_write_protect_fault (addr, write) : vm_handle_not_present_fault (f, addr, user, write);
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

	RETURN_VALUE_IF (rsp < 8, false);

	return fault_addr < (uintptr_t) USER_STACK &&
	       fault_addr >= (uintptr_t) (USER_STACK - STACK_MAX) &&
	       fault_addr >= rsp - 8;
}

// 스택 페이지 claim
static bool
vm_grow_stack_and_claim (void *addr) {
	vm_stack_growth (addr);
	return spt_find_page (&thread_current ()->spt, addr) != NULL;
}

// 권한 위반 fault 처리
static bool
vm_handle_write_protect_fault (void *addr, bool write) {
	RETURN_VALUE_IF (!write, false);
	struct page *page = spt_find_page (&thread_current ()->spt, addr);
	RETURN_VALUE_IF (page == NULL, false);

	return vm_handle_wp (page);
}

// 페이지 해제
void
vm_dealloc_page (struct page *page) {
	RETURN_IF (page == NULL);
	spt_entry_destroy (&spt_entry_from_page (page)->hash_elem, NULL);
}

// VA에 실제 메모리 올림
bool
vm_claim_page (void *va) {
	struct thread *curr = thread_current ();
	RETURN_VALUE_IF (va == NULL || curr == NULL, false);

	struct supplemental_page_table *spt = &curr->spt;
	struct page *page = spt_find_page (spt, va);
	RETURN_VALUE_IF (page == NULL, false);

	return vm_do_claim_page (page);
}

// PAGE 실제 메모리에 올리고 MMU 매핑
static bool
vm_do_claim_page (struct page *page) {
	struct thread *curr = thread_current ();
	struct frame *frame = vm_get_frame ();
	RETURN_VALUE_IF (page == NULL || frame == NULL || curr == NULL, false);

	frame->owner_thread = curr;
	frame->page = page;
	frame->owner = curr;
	page->frame = frame;

	if (!pml4_set_page (curr->pml4, page->va, frame->kva, page->writable)) {
		vm_destroy_page_frame (page);
		return false;
	}

	if (!swap_in (page, frame->kva)) {
		vm_destroy_page_frame (page);
		return false;
	}
	return true;
}

/* 새 보조 페이지 테이블을 초기화한다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	ASSERT (spt != NULL);
	hash_init (&spt->pages, spt_entry_hash, spt_entry_less, NULL);
}

// src 보조 페이지 테이블을 dst로 복사
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
                              struct supplemental_page_table *src) {
	struct hash_iterator i;

  struct hash_iterator i;
  hash_first (&i, &src->hash_table);
  while (hash_next (&i))
    {
      struct page *src_page = hash_entry (hash_cur (&i), struct page, hash_elem);
	  RETURN_FALSE_IF(!copy(dst, src_page));
	}
	return true;
}

static bool
spt_copy_page (struct supplemental_page_table *dst, struct page *page) {
	enum vm_type page_type;

	RETURN_VALUE_IF (page == NULL, false);

	if (page->operations->type == VM_UNINIT) {
		page_type = VM_TYPE (page->uninit.type);
		RETURN_VALUE_IF (page_type == VM_FILE, true);
		RETURN_VALUE_IF (page_type != VM_ANON, false);
		return spt_copy_uninit_page (page);
	}

	page_type = page_get_type (page);
	switch (page_type) {
	case VM_ANON:
		return spt_copy_anon_page (dst, page);
	case VM_FILE:
		return true;
	default:
		return false;
	}
}

static bool
spt_copy_uninit_page (struct page *page) {
	void *aux = NULL;

	RETURN_VALUE_IF (!spt_copy_lazy_aux (page, &aux), false);

	if (!vm_alloc_page_with_initializer (page->uninit.type, page->va,
	                                     page->writable, page->uninit.init, aux)) {
		spt_free_lazy_aux (aux);
		return false;
	}
	return true;
}

static bool
spt_copy_anon_page (struct supplemental_page_table *dst, struct page *page) {
	enum vm_type type;
	struct page *child_page;

	RETURN_VALUE_IF (dst == NULL || page == NULL, false);

	type = page->anon.type;
	RETURN_VALUE_IF (!vm_alloc_page (type, page->va, page->writable), false);

	child_page = spt_find_page (dst, page->va);
	RETURN_VALUE_IF (child_page == NULL, false);
	RETURN_VALUE_IF (!vm_do_claim_page (child_page), false);

	return anon_copy_page (child_page, page);
}

static bool
spt_copy_lazy_aux (struct page *page, void **aux) {
	struct lazy_load_arg *src_aux = page->uninit.aux;
	struct lazy_load_arg *dst_aux;

	*aux = NULL;
	RETURN_VALUE_IF (src_aux == NULL, true);

	dst_aux = malloc (sizeof *dst_aux);
	RETURN_VALUE_IF (dst_aux == NULL, false);

	memcpy (dst_aux, src_aux, sizeof *dst_aux);
	if (dst_aux->file != NULL) {
		dst_aux->file = file_reopen (dst_aux->file);
		if (dst_aux->file == NULL) {
			free (dst_aux);
			return false;
		}
	}

	*aux = dst_aux;
	return true;
}

static void
spt_free_lazy_aux (void *aux) {
	struct lazy_load_arg *lazy_aux = aux;

	RETURN_IF (lazy_aux == NULL);

	if (lazy_aux->file != NULL)
		file_close (lazy_aux->file);
	free (lazy_aux);
}

static void
vm_destroy_page_frame (struct page *page) {
	RETURN_IF (page == NULL || page->frame == NULL);

	struct frame *frame = page->frame;

	if (frame->owner != NULL && frame->owner->pml4 != NULL)
		pml4_clear_page (frame->owner->pml4, page->va);

	lock_acquire (&frame_table_lock);
	list_remove (&frame->frame_elem);
	lock_release (&frame_table_lock);

	if (frame->kva != NULL)
		palloc_free_page (frame->kva);

	frame->page = NULL;
	frame->owner = NULL;
	page->frame = NULL;
	free (frame);
}

// SPT 정리
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	RETURN_IF (spt == NULL);
	hash_destroy(&spt->hash_table, spt_page_destroy);
}

static void
spt_page_destroy (struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry (e, struct page, hash_elem);
	destroy (page);
	vm_destroy_page_frame (page);
	free (page);
}

static void
vm_destroy_page_frame (struct page *page) {
	RETURN_IF (page == NULL || page->frame == NULL);

	struct frame *frame = page->frame;

	lock_acquire (&frame_lock);
	if (clock_ptr == &frame->elem)
		clock_ptr = list_next(&frame->elem);
	list_remove (&frame->elem);
	lock_release (&frame_lock);

	if (frame->kva != NULL)
		palloc_free_page (frame->kva);

	pml4_clear_page(frame->owner_thread->pml4, page->va);
	frame->page = NULL;
	frame->owner_thread = NULL;
	free (frame);
	
}

