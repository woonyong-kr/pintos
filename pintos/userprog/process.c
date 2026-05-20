#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/cstr.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
static bool duplicate_fd_table (struct thread *curr, struct thread *parent);
static bool duplicate_running_file (struct thread *curr, struct thread *parent);
static void close_open_files (struct thread *curr);
static void reparent_or_reap_children (struct thread *curr);
static void finish_self_status (struct thread *curr);
static void close_running_file (struct thread *curr);
static void process_close_file_unlocked (int fd);
static void ensure_orphan_status_list (void);

static struct list orphan_status_list;
static bool orphan_status_list_initialized;

struct fork_args {
	struct thread *parent;   // fork를 호출한 부모 스레드
	struct intr_frame if_;   // 부모의 유저 레지스터 스냅샷
	struct child_status *cs; // 부모가 만든 자식 상태 레코드
};

struct initd_args {
	char *file_name;         // 실행할 프로그램 이름 복사본
	struct child_status *cs; // 부모가 만든 자식 상태 레코드
};

/* fd_table 최대 슬롯 수 (4KB 페이지 / 포인터 크기). */
#define FD_MAX (PGSIZE / sizeof (struct file *))

/*
 * initd와 다른 프로세스를 위한 일반적인 프로세스 초기화 함수.
 */
static void
process_init (void) {
	struct thread *current UNUSED = thread_current ();
}

tid_t
process_create_initd (const char *file_name) {
	struct thread *curr = thread_current ();
	char *file_name_copy;
	char program_name[THREAD_NAME_MAX];
	char *arg_start;
	struct child_status *cs = NULL;
	struct initd_args *args = NULL;
	tid_t tid = TID_ERROR;

	if (curr == NULL)
		return TID_ERROR;

	ensure_orphan_status_list ();

	/*
	 * 커널 풀에서 페이지 메모리 할당, 0 = 커널 영역(유저 옵션 없음)
	 */
	file_name_copy = palloc_get_page (0);
	if (file_name_copy == NULL)
		return TID_ERROR;
	strlcpy (file_name_copy, file_name, PGSIZE);

	strlcpy (program_name, file_name, sizeof program_name);
	arg_start = strchr (program_name, ' ');
	if (arg_start != NULL)
		*arg_start = '\0';

	cs = malloc (sizeof *cs);
	if (cs == NULL)
		goto done;

	cs->tid = TID_ERROR;
	cs->exit_status = -1;
	cs->waited = false;
	cs->exited = false;
	cs->orphaned = false;
	cs->fork_success = false;
	sema_init (&cs->fork_sema, 0);
	sema_init (&cs->wait_sema, 0);

	list_push_back (&curr->child_status_list, &cs->elem);

	args = malloc (sizeof *args);
	if (args == NULL)
		goto done;

	args->file_name = file_name_copy;
	args->cs = cs;

	tid = thread_create (program_name, PRI_DEFAULT, initd, args);
	if (tid == TID_ERROR)
		goto done;

	cs->tid = tid;
	return tid;
done:
	if (args != NULL)
		free (args);
	if (tid == TID_ERROR)
		palloc_free_page (file_name_copy);
	if (cs != NULL) {
		list_remove (&cs->elem);
		free (cs);
	}
	return TID_ERROR;
}

/*
 * 첫 번째 유저 프로세스를 시작하는 스레드 함수.
 */
static void
initd (void *f_name) {
	struct initd_args *args = f_name;
	struct thread *curr = thread_current ();

	if (curr == NULL || args == NULL)
		thread_exit ();

#ifdef VM
	supplemental_page_table_init (&curr->spt);
#endif

	if (thread_root () == NULL)
		thread_set_root (curr);

	curr->self_status = args->cs;
	curr->fd_table = palloc_get_page (PAL_ZERO);
	curr->next_fd = 2;

	process_init ();
	f_name = args->file_name;
	free (args);

	if (process_exec (f_name) < 0)
		thread_exit ();
	NOT_REACHED ();
}
/*
 * 현재 프로세스를 `name`이라는 이름으로 복제한다.
 * 새 프로세스의 스레드 id를 반환하고, 스레드를 생성할 수 없으면
 * TID_ERROR를 반환한다.
 */

tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct thread *curr = thread_current ();
	struct child_status *cs = NULL;
	struct fork_args *args = NULL;
	tid_t tid = TID_ERROR;

	if (curr == NULL)
		return TID_ERROR;

	cs = malloc (sizeof *cs);
	if (cs == NULL)
		goto done;

	cs->tid = TID_ERROR;
	cs->exit_status = -1;
	cs->waited = false;
	cs->exited = false;
	cs->orphaned = false;
	cs->fork_success = false;
	sema_init (&cs->fork_sema, 0);
	sema_init (&cs->wait_sema, 0);

	list_push_back (&curr->child_status_list, &cs->elem);

	args = malloc (sizeof *args);
	if (args == NULL)
		goto done;

	args->parent = curr;
	args->if_ = *if_;
	args->cs = cs;

	tid = thread_create (name, PRI_DEFAULT, __do_fork, args);
	if (tid == TID_ERROR)
		goto done;

	args = NULL;
	cs->tid = tid;
	sema_down (&cs->fork_sema);
	if (!cs->fork_success) {
		tid = TID_ERROR;
		goto done;
	}

	return tid;

done:
	if (args != NULL)
		free (args);
	if (cs != NULL) {
		list_remove (&cs->elem);
		free (cs);
	}
	return TID_ERROR;
}

#ifndef VM
/*
 * 이 함수를 pml4_for_each에 넘겨 부모의 주소 공간을 복제한다.
 * 이는 project 2에서만 사용된다.
 */
/* duplicate_pte - pml4_for_each 콜백. 부모의 PTE 하나를 자식에게 복제한다.
 *
 * 매개변수:
 *   pte - 부모의 페이지 테이블 엔트리 포인터
 *   va  - 이 PTE가 매핑하는 가상 주소
 *   aux - 부모 struct thread 포인터 (pml4_for_each에서 전달) */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	if (is_kernel_vaddr (va))
		return true;

	/* 2. 부모의 페이지 맵 레벨 4에서 VA를 해석한다. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
		return false;

	newpage = palloc_get_page (PAL_USER);
	if (newpage == NULL)
		return false;

	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable (pte);

	/* 5. 자식 페이지 테이블에 매핑 추가. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		palloc_free_page (newpage);
		return false;
	}
	return true;
}
#endif

static bool
duplicate_fd_table (struct thread *curr, struct thread *parent) {
	int fd;

	if (curr == NULL || parent == NULL)
		return false;

	curr->fd_table = palloc_get_page (PAL_ZERO);
	if (curr->fd_table == NULL)
		return false;

	if (parent->fd_table == NULL) {
		curr->next_fd = 2;
		return true;
	}

	lock_acquire (&filesys_lock);
	for (fd = 2; fd < parent->next_fd && fd < FD_MAX; fd++) {
		if (parent->fd_table[fd] == NULL)
			continue;

		curr->fd_table[fd] = file_duplicate (parent->fd_table[fd]);
		if (curr->fd_table[fd] == NULL) {
			lock_release (&filesys_lock);
			goto error;
		}
	}
	lock_release (&filesys_lock);

	curr->next_fd = parent->next_fd;
	return true;

error:
	lock_acquire (&filesys_lock);
	for (fd = 2; fd < FD_MAX; fd++) {
		if (curr->fd_table[fd] == NULL)
			continue;
		file_close (curr->fd_table[fd]);
		curr->fd_table[fd] = NULL;
	}
	lock_release (&filesys_lock);
	palloc_free_page (curr->fd_table);
	curr->fd_table = NULL;
	curr->next_fd = 2;
	return false;
}

static bool
duplicate_running_file (struct thread *curr, struct thread *parent) {
	if (curr == NULL || parent == NULL)
		return false;

	if (parent->running_file == NULL)
		return true;

	lock_acquire (&filesys_lock);
	curr->running_file = file_duplicate (parent->running_file);
	lock_release (&filesys_lock);
	return curr->running_file != NULL;
}

/*
 * 부모의 실행 문맥을 복사하는 스레드 함수.
 * 힌트) parent->tf에는 프로세스의 유저랜드 문맥이 들어 있지 않다.
 * 즉, process_fork의 두 번째 인자를 이 함수에 전달해야 한다.
 */
/* __do_fork - 자식 스레드의 진입점. 부모의 실행 문맥을 복제한다.
 *
 * 핵심 순서:
 *   1. 부모의 인터럽트 프레임(레지스터) 복사
 *   2. 부모의 페이지 테이블 복제 (duplicate_pte)
 *   3. 부모의 fd_table과 running_file 복제
 *   4. 자식의 fork 반환값을 0으로 설정
 *   5. 부모에게 "복제 완료" 알림, do_iret으로 유저 모드 진입 */

static void
__do_fork (void *aux) {
	struct fork_args *args = aux;
	struct thread *curr = thread_current ();
	struct thread *parent;
	struct intr_frame if_;

	if (curr == NULL || args == NULL)
		thread_exit ();

	parent = args->parent;
	if (parent == NULL) {
		free (args);
		thread_exit ();
	}

	curr->self_status = args->cs;
	memcpy (&if_, &args->if_, sizeof if_);
	free (args);

	curr->pml4 = pml4_create ();
	if (curr->pml4 == NULL)
		goto error;

	process_activate (curr);
#ifdef VM
	supplemental_page_table_init (&curr->spt);
	if (!supplemental_page_table_copy (&curr->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	if (!duplicate_fd_table (curr, parent))
		goto error;

	if (!duplicate_running_file (curr, parent))
		goto error;

	/* 자식 프로세스에서 fork()의 반환값은 0이다. */
	if_.R.rax = 0;

	process_init ();
	curr->self_status->fork_success = true;
	sema_up (&curr->self_status->fork_sema);

	/* 새로 생성된 프로세스로 전환한다. */
	do_iret (&if_);
error:
	if (curr->self_status != NULL) {
		struct child_status *cs = curr->self_status;

		cs->exit_status = -1;
		cs->exited = true;
		cs->fork_success = false;
		curr->self_status = NULL;
		sema_up (&cs->fork_sema);
	}
	thread_exit ();
}

/*
 * 현재 실행 문맥을 f_name으로 전환한다.
 * 실패하면 -1을 반환한다.
 */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;
	struct thread *curr = thread_current ();

	if (curr == NULL)
		return -1;

	/*
	 * thread 구조체에 있는 intr_frame은 사용할 수 없다.
	 * 현재 스레드가 다시 스케줄될 때 실행 정보를 그 멤버에 저장하기 때문이다.
	 */
	/*
	 * cs, ss, ds, es는 유저 모드
	 * eflags는 인터럽트 허용
	 */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/*
	 * 먼저 현재 문맥을 제거한다.
	 */
#ifdef VM
	process_cleanup ();
	close_running_file (curr);
	supplemental_page_table_init (&curr->spt);
#else
	close_running_file (curr);
	process_cleanup ();
#endif

	/*
	 * 그리고 바이너리를 로드한다.
	 */
	/*
	 * 바이너리 세팅
	 */
	success = load (file_name, &_if);
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/*
	 * 전환된 프로세스를 시작한다.
	 */
	/*
	 * 프로세스를 시작
	 */
	do_iret (&_if);
	NOT_REACHED ();
}

/*
 * 스레드 TID가 죽을 때까지 기다린 뒤 그 종료 상태를 반환한다.
 * 커널에 의해 종료되었다면(즉 예외 때문에 죽었다면) -1을 반환한다.
 * TID가 유효하지 않거나, 호출한 프로세스의 자식이 아니거나,
 * 해당 TID에 대해 process_wait()가 이미 성공적으로 호출된 적이 있다면
 * 기다리지 않고 즉시 -1을 반환한다.
 *
 * 이 함수는 문제 2-2에서 구현될 것이다. 현재는 아무 일도 하지 않는다.
 */

int
process_wait (tid_t child_tid) {
	struct thread *curr = thread_current ();
	int status;

	if (curr == NULL)
		return -1;

	struct list *childs = &curr->child_status_list;
	struct list_elem *e;
	struct child_status *cs;
	for (e = list_begin (childs); e != list_end (childs); e = list_next (e)) {
		cs = list_entry (e, struct child_status, elem);

		if (cs->tid != child_tid)
			continue;

		if (cs->waited)
			return -1;

		cs->waited = true;

		if (!cs->exited)
			sema_down (&cs->wait_sema);

		status = cs->exit_status;
		list_remove (&cs->elem);
		free (cs);
		return status;
	}
	return -1;
}

static void
close_open_files (struct thread *curr) {
	int fd;

	if (curr == NULL)
		return;

	for (fd = 2; fd < FD_MAX; fd++)
		process_close_file (fd);

	if (curr->fd_table != NULL) {
		palloc_free_page (curr->fd_table);
		curr->fd_table = NULL;
	}
}

static void
ensure_orphan_status_list (void) {
	if (!orphan_status_list_initialized) {
		list_init (&orphan_status_list);
		orphan_status_list_initialized = true;
	}
}

static void
reparent_or_reap_children (struct thread *curr) {
	struct list_elem *e;

	if (curr == NULL)
		return;

	ensure_orphan_status_list ();

	e = list_begin (&curr->child_status_list);
	while (e != list_end (&curr->child_status_list)) {
		struct child_status *cs = list_entry (e, struct child_status, elem);

		e = list_next (e);
		if (cs->exited && !cs->waited) {
			list_remove (&cs->elem);
			free (cs);
			continue;
		}

		cs->orphaned = true;
		list_remove (&cs->elem);
		list_push_back (&orphan_status_list, &cs->elem);
	}
}

static void
finish_self_status (struct thread *curr) {
	struct child_status *cs;

	if (curr == NULL || curr->self_status == NULL)
		return;

	cs = curr->self_status;
	if (!cs->exited) {
		cs->exited = true;
		sema_up (&cs->wait_sema);
	}

	if (cs->orphaned && !cs->waited) {
		list_remove (&cs->elem);
		free (cs);
		curr->self_status = NULL;
	}
}

/*
 * 프로세스를 종료한다. 이 함수는 thread_exit()에 의해 호출된다.
 */

void
process_exit_with_status (int status) {
	struct thread *curr = thread_current ();

	if (curr == NULL)
		thread_exit ();

	printf ("%s: exit(%d)\n", curr->name, status);

	if (curr->self_status != NULL)
		curr->self_status->exit_status = status;

	thread_exit ();
}

/* 프로세스가 보관하던 executable file을 닫아 실행 파일 write deny를 해제한다. */
static void
close_running_file (struct thread *curr) {
	if (curr == NULL || curr->running_file == NULL)
		return;

	lock_acquire (&filesys_lock);
	file_allow_write (curr->running_file);
	file_close (curr->running_file);
	lock_release (&filesys_lock);
	curr->running_file = NULL;
}

void
process_exit (void) {
	struct thread *curr = thread_current ();

	if (curr == NULL)
		return;

#ifdef VM
	process_cleanup ();
	close_running_file (curr);
	close_open_files (curr);
#else
	close_running_file (curr);
	close_open_files (curr);
#endif
	reparent_or_reap_children (curr);
	finish_self_status (curr);
	if (thread_root () == curr)
		thread_set_root (NULL);
#ifndef VM
	process_cleanup ();
#endif
}

int
process_add_file (struct file *f) {
	struct thread *curr = thread_current ();
	int fd;

	if (curr == NULL || f == NULL || curr->fd_table == NULL)
		return -1;

	// 빈 fd_table에 파일 f 넣는다
	for (fd = 2; fd < FD_MAX; fd++) {
		if (curr->fd_table[fd] != NULL)
			continue;

		curr->fd_table[fd] = f;

		// fd 번호가 next_fd보다 크거나 같으면
		if (fd >= curr->next_fd)
			curr->next_fd = fd + 1; // next_fd를 다음 번호로 바꿔라
		return fd;
	}
	// 빈 fd칸 못찾았을 때
	return -1;
}

struct file *
process_get_file (int fd) {
	struct thread *curr = thread_current ();

	if (curr == NULL || fd < 0 || fd >= FD_MAX || curr->fd_table == NULL)
		return NULL;

	return curr->fd_table[fd];
}

void
process_close_file (int fd) {
	lock_acquire (&filesys_lock);
	process_close_file_unlocked (fd);
	lock_release (&filesys_lock);
}

static void
process_close_file_unlocked (int fd) {
	struct thread *curr = thread_current ();

	if (curr == NULL || fd < 2 || fd >= FD_MAX || curr->fd_table == NULL)
		return;

	// fd 테이블에 파일이 없으면
	if (curr->fd_table[fd] == NULL)
		return;
	file_close (curr->fd_table[fd]);
	curr->fd_table[fd] = NULL;
	if (fd < curr->next_fd)
		curr->next_fd = fd;
}

/*
 * 현재 프로세스의 자원을 해제한다.
 */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/*
	 * 현재 프로세스의 페이지 디렉터리를 파괴하고 커널 전용 페이지 디렉터리로
	 * 되돌린다.
	 */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/*
		 * 여기서 순서는 매우 중요하다. 페이지 디렉터리를 전환하기 전에
		 * cur->pagedir를 NULL로 설정해야 타이머 인터럽트가 다시 프로세스의
		 * 페이지 디렉터리로 되돌아가지 않는다. 또한 프로세스의 페이지
		 * 디렉터리를 파괴하기 전에 기본 페이지 디렉터리를 활성화해야 한다.
		 * 그렇지 않으면 현재 활성 페이지 디렉터리가 이미 해제되어
		 * 비워진 것이 되어 버린다.
		 */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/*
 * 다음 스레드에서 유저 코드를 실행하도록 CPU를 설정한다.
 * 이 함수는 문맥 전환이 일어날 때마다 호출된다.
 */
void
process_activate (struct thread *next) {
	pml4_activate (next->pml4);
	tss_update (next);
}

/*
 * 우리는 ELF 바이너리를 로드한다. 다음 정의들은 ELF 명세 [ELF1]에서
 * 거의 그대로 가져온 것이다.
 */

/*
 * ELF 타입들. [ELF1] 1-2를 참고하라.
 */
#define EI_NIDENT 16

/*
 * 무시한다.
 */
#define PT_NULL 0
/*
 * 로드 가능한 세그먼트.
 */
#define PT_LOAD 1
/*
 * 동적 링크 정보.
 */
#define PT_DYNAMIC 2
/*
 * 동적 로더의 이름.
 */
#define PT_INTERP 3
/*
 * 보조 정보.
 */
#define PT_NOTE 4
/*
 * 예약됨.
 */
#define PT_SHLIB 5
/*
 * 프로그램 헤더 테이블.
 */
#define PT_PHDR 6
/*
 * 스택 세그먼트.
 */
#define PT_STACK 0x6474e551

/*
 * 실행 가능.
 */
#define PF_X 1
/*
 * 쓰기 가능.
 */
#define PF_W 2
/*
 * 읽기 가능.
 */
#define PF_R 4

/*
 * 실행 파일 헤더. [ELF1] 1-4부터 1-8까지를 참고하라.
 * 이는 ELF 바이너리의 맨 앞에 위치한다.
 */
/*
 * ELF64 헤더는 실행 파일의 맨 앞 64바이트에 위치한다.
 * 이 영역은 파일 전체를 읽기 전에 먼저 확인하는 요약 정보다.
 *
 * 바이트 배치는 다음과 같다.
 * 0x00 ~ 0x0f : e_ident
 *               ELF 매직 값, 64비트 형식, 엔디안 같은 기본 식별 정보
 * 0x10 ~ 0x11 : e_type
 *               실행 파일인지, 목적 파일인지 같은 파일 종류
 * 0x12 ~ 0x13 : e_machine
 *               대상 CPU 종류, 여기서는 x86-64(0x3E)
 * 0x14 ~ 0x17 : e_version
 *               ELF 버전
 * 0x18 ~ 0x1f : e_entry
 *               적재가 끝난 뒤 처음 실행할 가상 주소
 * 0x20 ~ 0x27 : e_phoff
 *               프로그램 헤더 표가 파일에서 시작하는 위치
 * 0x28 ~ 0x2f : e_shoff
 *               섹션 헤더 표가 파일에서 시작하는 위치
 * 0x30 ~ 0x33 : e_flags
 *               추가 플래그
 * 0x34 ~ 0x35 : e_ehsize
 *               이 ELF 헤더 자체의 크기
 * 0x36 ~ 0x37 : e_phentsize
 *               프로그램 헤더 1개의 크기
 * 0x38 ~ 0x39 : e_phnum
 *               프로그램 헤더 개수
 * 0x3a ~ 0x3b : e_shentsize
 *               섹션 헤더 1개의 크기
 * 0x3c ~ 0x3d : e_shnum
 *               섹션 헤더 개수
 * 0x3e ~ 0x3f : e_shstrndx
 *               섹션 이름 문자열 표의 인덱스
 */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/*
 * 약어.
 */
#define ELF  ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
static bool setup_process_address_space (struct thread *t);
static char *copy_command_line (const char *file_name);
static bool parse_command_line (char *command, char **argv, int *argc,
                                size_t argv_cap);
static struct file *open_executable (const char *program_name);
static bool read_elf_header (struct file *file, struct ELF *ehdr);
static bool load_program_headers (struct file *file, const struct ELF *ehdr);
static bool load_loadable_segment (struct file *file,
                                   const struct Phdr *phdr);
static bool setup_initial_stack (struct intr_frame *if_, char **argv,
                                 int argc);

/* PHDR이 유효한 로드 가능한 세그먼트인지 확인하고 true를 반환한다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr은 동일한 페이지 오프셋을 가져야 한다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 파일 내부에 있어야 한다. */
	lock_acquire (&filesys_lock);
	off_t length = file_length (file);
	lock_release (&filesys_lock);
	if (phdr->p_offset > (uint64_t) length)
		return false;

	/* p_memsz는 p_filesz보다 크거나 같아야 한다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 세그먼트는 가상 메모리의 유저 영역 내에 위치해야 한다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 주소 영역이 감싸지는 형태가 아니어야 한다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* 0번 페이지는 매핑하지 않는다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	return true;
}

static bool
setup_process_address_space (struct thread *t) {
	t->pml4 = pml4_create ();
	RETURN_VALUE_IF (t->pml4 == NULL, false);

	process_activate (t);
	return true;
}

static char *
copy_command_line (const char *file_name) {
	RETURN_VALUE_IF (file_name == NULL, NULL);

	char *copy = palloc_get_page (0);

	RETURN_VALUE_IF (copy == NULL, NULL);

	strlcpy (copy, file_name, PGSIZE);
	return copy;
}

static bool
parse_command_line (char *command, char **argv, int *argc, size_t argv_cap) {
	char *token;
	char *save_point;

	RETURN_VALUE_IF (command == NULL || argv == NULL || argc == NULL, false);

	*argc = 0;
	for (token = strtok_r (command, " ", &save_point); token != NULL;
	     token = strtok_r (NULL, " ", &save_point)) {
		if ((size_t) *argc >= argv_cap)
			return false;
		argv[(*argc)++] = token;
	}
	return *argc > 0;
}

static struct file *
open_executable (const char *program_name) {
	struct file *file;

	lock_acquire (&filesys_lock);
	file = filesys_open (program_name);
	if (file != NULL)
		file_deny_write (file);
	lock_release (&filesys_lock);

	return file;
}

static bool
read_elf_header (struct file *file, struct ELF *ehdr) {
	bool read_success;

	lock_acquire (&filesys_lock);
	read_success = file_read (file, ehdr, sizeof *ehdr) == sizeof *ehdr;
	lock_release (&filesys_lock);

	return read_success &&
	       memcmp (ehdr->e_ident, "\177ELF\2\1\1", 7) == 0 &&
	       ehdr->e_type == 2 &&
	       ehdr->e_machine == 0x3E &&
	       ehdr->e_version == 1 &&
	       ehdr->e_phentsize == sizeof (struct Phdr) &&
	       ehdr->e_phnum <= 1024;
}

static bool
read_program_header (struct file *file, off_t file_ofs, struct Phdr *phdr) {
	off_t length;
	bool read_success;

	lock_acquire (&filesys_lock);
	length = file_length (file);
	lock_release (&filesys_lock);
	if (file_ofs < 0 || file_ofs > length)
		return false;

	lock_acquire (&filesys_lock);
	file_seek (file, file_ofs);
	read_success = file_read (file, phdr, sizeof *phdr) == sizeof *phdr;
	lock_release (&filesys_lock);

	return read_success;
}

static bool
load_loadable_segment (struct file *file, const struct Phdr *phdr) {
	bool writable;
	uint64_t file_page;
	uint64_t mem_page;
	uint64_t page_offset;
	uint32_t read_bytes;
	uint32_t zero_bytes;

	if (!validate_segment (phdr, file))
		return false;

	writable = (phdr->p_flags & PF_W) != 0;
	file_page = phdr->p_offset & ~PGMASK;
	mem_page = phdr->p_vaddr & ~PGMASK;
	page_offset = phdr->p_vaddr & PGMASK;

	if (phdr->p_filesz > 0) {
		read_bytes = page_offset + phdr->p_filesz;
		zero_bytes = ROUND_UP (page_offset + phdr->p_memsz, PGSIZE) -
		             read_bytes;
	} else {
		read_bytes = 0;
		zero_bytes = ROUND_UP (page_offset + phdr->p_memsz, PGSIZE);
	}

	return load_segment (file, file_page, (void *) mem_page, read_bytes,
	                     zero_bytes, writable);
}

static bool
load_program_headers (struct file *file, const struct ELF *ehdr) {
	off_t file_ofs = ehdr->e_phoff;

	for (int i = 0; i < ehdr->e_phnum; i++) {
		struct Phdr phdr;

		if (!read_program_header (file, file_ofs, &phdr))
			return false;
		file_ofs += sizeof phdr;

		switch (phdr.p_type) {
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			return false;
		case PT_LOAD:
			if (!load_loadable_segment (file, &phdr))
				return false;
			break;
		}
	}
	return true;
}

static bool
setup_initial_stack (struct intr_frame *if_, char **argv, int argc) {
	char *stack_p;

	if (!setup_stack (if_))
		return false;

	stack_p = (char *) if_->rsp;
	for (int argi = argc - 1; argi >= 0; argi--) {
		int size = CSTR_SIZE (argv[argi]);
		stack_p -= size;
		memcpy (stack_p, argv[argi], size);
		argv[argi] = stack_p;
	}

	stack_p = (char *) ((uintptr_t) stack_p & -8);
	stack_p -= sizeof (uintptr_t);
	memset (stack_p, 0, sizeof (uintptr_t));

	for (int n = argc - 1; n >= 0; n--) {
		stack_p -= sizeof (uintptr_t);
		*(uintptr_t *) stack_p = (uintptr_t) argv[n];
	}

	if_->R.rsi = (uint64_t) stack_p;
	if_->R.rdi = argc;

	stack_p -= sizeof (uintptr_t);
	memset (stack_p, 0, sizeof (uintptr_t));
	if_->rsp = (uintptr_t) stack_p;
#ifdef VM
	thread_current ()->user_rsp = if_->rsp;
#endif
	return true;
}

/*
 * FILE_NAME의 ELF 실행 파일을 현재 스레드에 로드한다.
 * 실행 파일의 진입점을 *RIP에 저장하고,
 * 초기 스택 포인터를 *RSP에 저장한다.
 * 성공하면 true, 그렇지 않으면 false를 반환한다.
 */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	/*
	 * ELF : 현재 실행 파일의 ELF64 헤더 저장 변수
	 */
	struct ELF ehdr;
	struct file *file = NULL;
	bool success = false;
	char *argv[64];
	char *fn_copy = NULL;
	int argc = 0;
	
	if(!setup_process_address_space (t))
		goto done;

	fn_copy = copy_command_line (file_name);

	if (fn_copy == NULL)
		goto done;

	if(!parse_command_line (fn_copy, argv, &argc, sizeof argv / sizeof *argv))
		goto done;

	file = open_executable (argv[0]);
	
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	if (!read_elf_header (file, &ehdr)) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	if (!load_program_headers (file, &ehdr))
		goto done;
	
	if(!setup_initial_stack (if_, argv, argc))
		goto done;

	if_->rip = ehdr.e_entry;
	success = true;


done:
	if (fn_copy != NULL)
		palloc_free_page (fn_copy);

	if (success && file != NULL) {
		t->running_file = file;
		file = NULL;
	}

	if (file != NULL) {
		lock_acquire (&filesys_lock);
		file_close (file);
		lock_release (&filesys_lock);
	}
	return success;
}

#ifndef VM
/*
 * 이 블록의 코드는 project 2 동안에만 사용된다.
 * project 2 전체를 위한 함수를 구현하고 싶다면 #ifndef 매크로 바깥에 구현하라.
 */

/*
 * load() 보조 함수들.
 */
static bool install_page (void *upage, void *kpage, bool writable);

/*
 * FILE의 OFS 오프셋에서 시작하는 세그먼트를 UPAGE 주소에 로드한다.
 * 전체적으로 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 아래와 같이
 * 초기화된다.
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE의 OFS 오프셋부터 읽어 와야 한다.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 채워야 한다.
 *
 * 이 함수가 초기화한 페이지들은 WRITABLE이 true면 유저 프로세스가
 * 쓸 수 있어야 하고, 그렇지 않으면 읽기 전용이어야 한다.
 *
 * 성공하면 true를, 메모리 할당 오류나 디스크 읽기 오류가 발생하면 false를
 * 반환한다.
 */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/*
		 * 이 페이지를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고
		 * 마지막 PAGE_ZERO_BYTES 바이트를 0으로 채운다.
		 */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/*
		 * 메모리 페이지 하나를 얻는다.
		 */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/*
		 * 이 페이지를 로드한다.
		 */
		if (page_read_bytes > 0) {
			lock_acquire (&filesys_lock);
			off_t bytes_read =
			        file_read_at (file, kpage, page_read_bytes, ofs);
			lock_release (&filesys_lock);
			if (bytes_read != (off_t) page_read_bytes) {
				palloc_free_page (kpage);
				return false;
			}
			ofs += page_read_bytes;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/*
		 * 이 페이지를 프로세스의 주소 공간에 추가한다.
		 */
		if (!install_page (upage, kpage, writable)) {
			printf ("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/*
		 * 다음 페이지로 진행한다.
		 */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/*
 * USER_STACK에 0으로 채워진 페이지를 매핑해 최소한의 스택을 만든다.
 */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success =
		        install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/*
 * 유저 가상 주소 UPAGE에서 커널 가상 주소 KPAGE로의 매핑을 페이지 테이블에
 * 추가한다.
 * WRITABLE이 true면 유저 프로세스가 이 페이지를 수정할 수 있고,
 * 그렇지 않으면 읽기 전용이다.
 * UPAGE는 이미 매핑되어 있으면 안 된다.
 * KPAGE는 아마도 palloc_get_page()로 유저 풀에서 얻은 페이지여야 한다.
 * 성공하면 true를, UPAGE가 이미 매핑되어 있거나 메모리 할당에 실패하면
 * false를 반환한다.
 */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/*
	 * 그 가상 주소에 이미 페이지가 없는지 확인한 뒤, 그 위치에 페이지를
	 * 매핑한다.
	 */
	return (pml4_get_page (t->pml4, upage) == NULL &&
	        pml4_set_page (t->pml4, upage, kpage, writable));
}

#else
/*
 * 여기부터의 코드는 project 3 이후에 사용된다.
 * project 2만을 위한 함수를 구현하려면 위쪽 블록에 구현하라.
 */

static bool
lazy_load_segment (struct page *page, void *aux_) {
	struct lazy_load_arg *aux = aux_;
	uint8_t *kva = page->frame->kva;
	bool success = false;

	if (aux == NULL || aux->file == NULL)
		goto done;

	lock_acquire (&filesys_lock);
	if (file_read_at (aux->file, kva, aux->page_read_bytes, aux->ofs) == (off_t) aux->page_read_bytes) {
		memset (kva + aux->page_read_bytes, 0, aux->page_zero_bytes);
		success = true;
	}
	lock_release (&filesys_lock);

done:
	if (aux != NULL) {
		if (aux->file != NULL) {
			lock_acquire (&filesys_lock);
			file_close (aux->file);
			lock_release (&filesys_lock);
		}
		free (aux);
	}
	return success;
}

/*
 * FILE의 OFS 오프셋에서 시작하는 세그먼트를 UPAGE 주소에 로드한다.
 * 전체적으로 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 아래와 같이
 * 초기화된다.
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE의 OFS 오프셋부터 읽어 와야 한다.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 채워야 한다.
 *
 * 이 함수가 초기화한 페이지들은 WRITABLE이 true면 유저 프로세스가
 * 쓸 수 있어야 하고, 그렇지 않으면 읽기 전용이어야 한다.
 *
 * 성공하면 true를, 메모리 할당 오류나 디스크 읽기 오류가 발생하면 false를
 * 반환한다.
 */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0); 
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/*
		 * 이 페이지를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고
		 * 마지막 PAGE_ZERO_BYTES 바이트를 0으로 채운다.
		 */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct lazy_load_arg * aux = malloc(sizeof *aux);
		RETURN_VALUE_IF (aux == NULL, false);

		aux->file = file_reopen(file);
		if (aux->file == NULL) {
			free (aux);
			return false;
		}
		aux->ofs = ofs;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = page_zero_bytes;
		
		if (!vm_alloc_page_with_initializer (VM_ANON, upage, writable,
		                                     lazy_load_segment, aux)) {
			file_close (aux->file);
			free(aux);
			return false;																		
		}	
		/*
		 * 다음 페이지로 진행한다.
		 */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return true;
}

/* USER_STACK에 스택용 PAGE를 만든다. 성공하면 true를 반환한다. */
static bool
setup_stack (struct intr_frame *if_) {
	struct thread *curr = thread_current ();
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	RETURN_VALUE_IF (if_ == NULL, false);
	RETURN_VALUE_IF (curr == NULL, false);

	curr->stack_bottom = stack_bottom;
	RETURN_VALUE_IF (!vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true), false);
	RETURN_VALUE_IF (!vm_claim_page (stack_bottom), false);

	if_->rsp = USER_STACK;

	return true;
}
#endif /* VM */
