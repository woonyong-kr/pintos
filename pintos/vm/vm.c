/* vm.c: Generic interface for virtual memory objects. */

#include <string.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "hash.h"
#include "userprog/process.h"
#include "threads/mmu.h"
#define STACK_MAX ((uintptr_t) 1 << 20) /* 유저 스택 최대 크기: 1MB */

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem* clock_ptr;

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
		default:
			return ty;
	}
}

/* 보조 함수들 */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
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

static void
vm_destroy_page_frame (struct page *page);
static void
spt_entry_destroy (struct hash_elem *e, void *aux UNUSED);

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
}

/* 교체 대상이 될 struct frame을 고른다. */
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
		// RETURN_IF (!vm_claim_page (stack_bottom)); claim 2번 들어감

		curr->stack_bottom = stack_bottom;
	}
}

/* 쓰기 보호 페이지에서 발생한 폴트를 처리한다. */
static bool
vm_handle_wp (struct page *page UNUSED) {
	return false;
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

	frame->owner_thread = curr;
	frame->page = page;
	page->frame = frame;

	if (!pml4_set_page (curr->pml4, page->va, frame->kva, page->writable)) {
		frame->page = NULL;
		page->frame = NULL;
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
  hash_first (&i, &src->hash_table);
  while (hash_next (&i))
    {
      struct page *f = hash_entry (hash_cur (&i), struct page, hash_elem);
      enum vm_type page_type = VM_TYPE (f->operations->type);
      if (page_type == VM_UNINIT)
        {
          void *aux = f->uninit.aux;
          if (aux)
            {
              struct lazy_load_arg *temp_aux
                  = (struct lazy_load_arg *)malloc (sizeof (struct lazy_load_arg));
              memcpy (temp_aux, f->uninit.aux, sizeof (struct lazy_load_arg));
              if (temp_aux->file)
                {
                  temp_aux->file = file_reopen (temp_aux->file);
                  if (temp_aux->file == NULL)
                    {
                      free (temp_aux);
                      return false;
                    }
                }
              aux = temp_aux;
            }

          if (!vm_alloc_page_with_initializer (
                  f->uninit.type, f->va, f->writable,
                  f->uninit.page_initializer, aux))
            return false;
        }
      else
        {
          if (!vm_alloc_page (page_type, f->va, f->writable))
            return false;
          struct page *child_page = spt_find_page (dst, f->va);
          if (child_page == NULL)
            return false;
          if (!vm_do_claim_page (child_page))
            return false;
          memcpy (child_page->frame->kva, f->frame->kva, PGSIZE);
        }
    }
  return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	RETURN_IF (spt == NULL);
	hash_destroy(&spt->hash_table, spt_entry_destroy);
}

static void
spt_entry_destroy (struct hash_elem *e, void *aux UNUSED) {
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
		clock_ptr = list_next(&frame_table);
	list_remove (&frame->elem);
	lock_release (&frame_lock);

	if (frame->kva != NULL)
		palloc_free_page (frame->kva);

	pml4_clear_page(frame->owner_thread->pml4, page->va);
	frame->page = NULL;
	frame->owner_thread = NULL;
	free (frame);
	
}
