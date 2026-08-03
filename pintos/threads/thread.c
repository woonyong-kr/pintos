#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "devices/timer.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/*
 * struct thread의 `magic` 멤버에 넣는 임의 값.
 * 스택 오버플로우를 감지하는 데 사용된다.
 * 자세한 내용은 thread.h 상단의 큰 주석을 참고하라.
 */
#define THREAD_MAGIC 0xcd6abf4b

/*
 * 기본 스레드를 위한 임의 값.
 * 이 값은 수정하지 말라.
 */
#define THREAD_BASIC 0xd42df210

/*
 * THREAD_READY 상태의 프로세스 리스트.
 * 즉, 실행할 준비는 되었지만 실제로 실행 중은 아닌 프로세스들이다.
 */
static struct list ready_list;

/* MLFQS가 blocked 스레드까지 갱신할 수 있도록 모든 스레드를 추적한다. */
static struct list all_list;

/* MLFQS 17.14 고정소수점 표현과 시스템 load average. */
#define FP_SHIFT 14
#define FP_FACTOR (1 << FP_SHIFT)
static int load_avg;

/* 바쁜 대기 문제를 줄이기 위해 sleep_list를 선언한다. */
static struct list sleep_list;

/*
 * idle 스레드.
 */
static struct thread *idle_thread;

/*
 * 초기 스레드, 즉 init.c:main()을 실행하는 스레드.
 */
static struct thread *initial_thread;

#ifdef USERPROG
/* 최초 유저 프로세스를 실행하는 루트 스레드. */
static struct thread *root_thread;
#endif

/*
 * allocate_tid()에서 사용하는 락.
 */
static struct lock tid_lock;

/* 스레드 파괴 요청들. */
static struct list destruction_req;

/*
 * 통계 정보.
 */
/*
 * idle 상태에서 소비한 타이머 틱 수.
 */
static long long idle_ticks;
/*
 * 커널 스레드에서 소비한 타이머 틱 수.
 */
static long long kernel_ticks;
/*
 * 유저 프로그램에서 소비한 타이머 틱 수.
 */
static long long user_ticks;

/*
 * 스케줄링 관련 값들.
 */
/*
 * 각 스레드에 줄 타이머 틱 수.
 */
#define TIME_SLICE 4
/*
 * 마지막 양보 이후 지난 타이머 틱 수.
 */
static unsigned thread_ticks;

/*
 * false(기본값)면 라운드 로빈 스케줄러를 사용한다.
 * true면 다단계 피드백 큐 스케줄러를 사용한다.
 * 커널 명령줄 옵션 "-o mlfqs"로 제어된다.
 */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule (int status);
static void schedule (void);
static tid_t allocate_tid (void);
static int fp_from_int (int n);
static int fp_to_int_zero (int x);
static int fp_to_int_nearest (int x);
static int fp_add (int x, int y);
static int fp_add_int (int x, int n);
static int fp_mul (int x, int y);
static int fp_mul_int (int x, int n);
static int fp_div (int x, int y);
static int fp_div_int (int x, int n);
static void mlfqs_update_load_avg (void);
static void mlfqs_update_recent_cpu (struct thread *t);
static void mlfqs_update_priority (struct thread *t);
static void mlfqs_update_all_recent_cpu (void);
static void mlfqs_update_all_priorities (void);

/* list_push_ordered, cmp_priority를 제거하고
 * list_insert_ordered + thread_priority로 통일했다.
 */

/*
 * T가 유효한 스레드를 가리키는 것처럼 보이면 true를 반환한다.
 */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/*
 * 현재 실행 중인 스레드를 반환한다.
 * CPU의 스택 포인터 `rsp`를 읽고, 그것을 페이지 시작 주소까지 내림한다.
 * `struct thread`는 항상 페이지의 시작 부분에 있고 스택 포인터는 그 중간
 * 어딘가에 있으므로, 이렇게 하면 현재 스레드를 찾을 수 있다.
 */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))

/* thread_start를 위한 전역 디스크립터 테이블이다.
 * gdt는 thread_init 이후에 설정되므로, 먼저 임시 gdt를 준비해야 한다.
 */
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/*
 * 현재 실행 중인 코드를 스레드로 바꾸어 스레딩 시스템을 초기화한다.
 * 일반적으로 항상 가능한 일은 아니며, 이 경우 loader.S가 스택의 바닥을
 * 페이지 경계에 맞게 두었기 때문에 가능하다.
 *
 * 또한 run queue와 tid 락도 초기화한다.
 *
 * 이 함수를 호출한 뒤에는 thread_create()로 스레드를 만들기 전에
 * 반드시 페이지 할당기를 초기화해야 한다.
 *
 * 이 함수가 끝나기 전까지는 thread_current()를 호출하는 것이 안전하지 않다.
 */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 임시 커널 gdt를 다시 로드한다.
	 * 이 gdt에는 유저 문맥이 포함되어 있지 않다.
	 * 커널은 gdt_init()에서 유저 문맥을 포함한 gdt를 다시 구성한다.
	 */
	struct desc_ptr gdt_ds = { .size = sizeof (gdt) - 1,
		                       .address = (uint64_t) gdt };
	lgdt (&gdt_ds);

	/* 전역 스레드용 객체 초기화(sleep_list 추가) */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&sleep_list);
	list_init (&destruction_req);
	list_init (&all_list);
	load_avg = 0;

	/*
	 * 현재 실행 중인 스레드용 thread 구조체를 설정한다.
	 */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	if (thread_mlfqs)
		mlfqs_update_priority (initial_thread);
	list_push_back (&all_list, &initial_thread->all_elem);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/*
 * 인터럽트를 활성화해 선점형 스레드 스케줄링을 시작한다.
 * 또한 idle 스레드도 생성한다.
 */
void
thread_start (void) {
	/*
	 * idle 스레드를 생성한다.
	 */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/*
	 * 선점형 스레드 스케줄링을 시작한다.
	 */
	intr_enable ();

	/*
	 * idle 스레드가 idle_thread를 초기화할 때까지 기다린다.
	 */
	sema_down (&idle_started);
}

/*
 * 각 타이머 틱마다 타이머 인터럽트 핸들러에 의해 호출된다.
 * 따라서 이 함수는 외부 인터럽트 문맥에서 실행된다.
 */
void
thread_tick (void) {
	struct thread *t = thread_current ();
	int64_t ticks = timer_ticks ();

	/*
	 * 통계를 갱신한다.
	 */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	if (thread_mlfqs) {
		if (t != idle_thread)
			t->recent_cpu = fp_add_int (t->recent_cpu, 1);

		if (ticks % TIMER_FREQ == 0) {
			mlfqs_update_load_avg ();
			mlfqs_update_all_recent_cpu ();
		}

		if (ticks % TIME_SLICE == 0) {
			mlfqs_update_all_priorities ();
			list_sort (&ready_list, thread_priority, NULL);
			if (!list_empty (&ready_list)) {
				struct thread *front = list_entry (list_front (&ready_list),
				                                  struct thread, elem);
				if (front->priority > t->priority)
					intr_yield_on_return ();
			}
		}
	}

	/*
	 * 선점을 강제한다.
	 */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/*
 * 스레드 통계를 출력한다.
 */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
	        idle_ticks, kernel_ticks, user_ticks);
}

tid_t
thread_create (const char *name, int priority, thread_func *function,
               void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/*
	 * 할당한 페이지 t에 새 스레드 구조체 초기화
	 */
	init_thread (t, name, priority);
	if (thread_mlfqs && function != idle) {
		t->nice = thread_current ()->nice;
		t->recent_cpu = thread_current ()->recent_cpu;
		mlfqs_update_priority (t);
	}

	/*
	 * rip : 새 스레드가 처음 실행할 함수 주소
	 * R.rdi : kernel_thread에 넘길 첫 번째 인자(function)
	 * R.rsi : kernel_thread에 넘길 두 번째 인자(aux)
	 * ds : 읽고 쓸 데이터 권한 정보, 0이면 커널 3이면 유저
	 * es : 읽고 쓸 데이터 권한 정보, 0이면 커널 3이면 유저
	 * ss : 사용할 스택 권한 정보, 0이면 커널 3이면 유저
	 * cs : 실행 권한 정보, 0이면 커널 3이면 유저
	 * eflags : 인터럽트 허용 여부, CPU 실행 옵션 값
	 */
	tid = t->tid = allocate_tid ();
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* all_list는 타이머 인터럽트에서도 순회하므로 원자적으로 연결한다. */
	enum intr_level old_level = intr_disable ();
	list_push_back (&all_list, &t->all_elem);
	intr_set_level (old_level);

	/*
	 * 새 스레드를 스케줄러 ready_list 추가
	 */
	thread_unblock (t);

	/*
	 * 새 스레드의 우선순위가 더 높으면 CPU 양보
	 */
	if (priority > thread_current ()->priority) {
		thread_yield ();
	}
	return tid;
}

/*
 * 현재 스레드를 잠들게 한다.
 * thread_unblock()에 의해 깨어날 때까지 다시 스케줄되지 않는다.
 *
 * 이 함수는 인터럽트가 꺼진 상태에서 호출되어야 한다.
 * 보통은 synch.h의 동기화 프리미티브 중 하나를 사용하는 편이 더 낫다.
 */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/*
 * blocked 상태의 스레드 T를 실행 가능한 ready 상태로 전환한다.
 * T가 blocked 상태가 아니라면 오류다.
 * (실행 중인 스레드를 ready 상태로 만들려면 thread_yield()를 사용하라.)
 *
 * 이 함수는 현재 실행 중인 스레드를 선점하지 않는다.
 * 이는 중요할 수 있다. 호출자가 직접 인터럽트를 꺼둔 상태였다면,
 * 스레드를 원자적으로 unblock하고 다른 데이터를 함께 갱신할 수 있기를
 * 기대할 수 있기 때문이다.
 */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	// list_push_back (&ready_list, &t->elem);
	list_insert_ordered (&ready_list, &t->elem, thread_priority, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/*
 * 현재 실행 중인 스레드의 이름을 반환한다.
 */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/*
 * 현재 실행 중인 스레드를 반환한다.
 * running_thread()에 몇 가지 sanity check를 더한 것이다.
 * 자세한 내용은 thread.h 상단의 큰 주석을 참고하라.
 */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/*
	 * T가 정말 스레드인지 확인한다.
	 * 이 assertion들 중 하나라도 터지면, 해당 스레드는 스택 오버플로우를
	 * 일으켰을 가능성이 있다. 각 스레드는 4 kB보다 작은 스택만 가지므로, 큰
	 * 자동 배열 몇 개나 적당한 수준의 재귀만으로도 스택 오버플로우가 날 수
	 * 있다.
	 */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

struct thread *
thread_root (void) {
#ifdef USERPROG
	return root_thread;
#else
	return NULL;
#endif
}

void
thread_set_root (struct thread *t) {
#ifdef USERPROG
	root_thread = t;
#else
	(void) t;
#endif
}

/*
 * 현재 실행 중인 스레드의 tid를 반환한다.
 */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/*
 * 현재 스레드를 스케줄 대상에서 제외하고 파괴한다.
 * 호출자에게는 절대 반환하지 않는다.
 */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/*
	 * 상태만 dying으로 바꾸고 다른 프로세스를 스케줄한다.
	 * 실제 파괴는 schedule_tail() 호출 중에 이루어진다.
	 */

	intr_disable ();
	list_remove (&thread_current ()->all_elem);
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/*
 * CPU를 양보한다.
 * 현재 스레드는 sleep 상태가 되지 않으며, 스케줄러 판단에 따라 곧바로 다시
 * 스케줄될 수도 있다.
 */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered (&ready_list, &curr->elem, thread_priority, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/*
 * 현재 스레드의 우선순위를 NEW_PRIORITY로 설정한다.
 */
void
thread_set_priority (int new_priority) {
	if (thread_mlfqs)
		return;
	struct thread *tcur = thread_current ();
	tcur->base_priority = new_priority;
	refresh_priority (tcur);
	check_preemption ();
}

/*
 * 현재 스레드의 우선순위를 반환한다.
 */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/*
 * 현재 스레드의 nice 값을 NICE로 설정한다.
 */
void
thread_set_nice (int nice) {
	ASSERT (-20 <= nice && nice <= 20);
	struct thread *t = thread_current ();
	t->nice = nice;
	if (thread_mlfqs) {
		mlfqs_update_priority (t);
		check_preemption ();
	}
}

/*
 * 현재 스레드의 nice 값을 반환한다.
 */
int
thread_get_nice (void) {
	return thread_current ()->nice;
}

/*
 * 시스템 load average의 100배 값을 반환한다.
 */
int
thread_get_load_avg (void) {
	return fp_to_int_nearest (fp_mul_int (load_avg, 100));
}

/*
 * 현재 스레드 recent_cpu 값의 100배를 반환한다.
 */
int
thread_get_recent_cpu (void) {
	return fp_to_int_nearest (fp_mul_int (thread_current ()->recent_cpu, 100));
}

/*
 * idle 스레드.
 * 다른 어떤 스레드도 실행할 준비가 되어 있지 않을 때 실행된다.
 *
 * idle 스레드는 처음에 thread_start()에 의해 ready list에 들어간다.
 * 처음 한 번 스케줄되면 idle_thread를 초기화하고,
 * thread_start()가 계속 진행할 수 있도록 전달받은 세마포어를 up한 뒤
 * 즉시 block된다. 그 이후로 idle 스레드는 ready list에 다시 나타나지 않는다.
 * ready list가 비어 있을 때 next_thread_to_run()이 특수 경우로 반환한다.
 */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드가 실행되도록 한다. */
		intr_disable ();
		thread_block ();

		/*
		 * 인터럽트를 다시 켜고 다음 인터럽트를 기다린다.
		 *
		 * `sti` 명령은 다음 명령이 끝날 때까지 인터럽트를 막으므로,
		 * 이 두 명령은 원자적으로 실행된다. 이 원자성은 중요하다.
		 * 그렇지 않으면 인터럽트를 다시 켠 시점과 다음 인터럽트를 기다리는
		 * 시점 사이에 인터럽트가 처리되어, 최대 한 클럭 틱만큼의 시간이 낭비될
		 * 수 있다.
		 *
		 * [IA32-v2a]의 "HLT", [IA32-v2b]의 "STI",
		 * [IA32-v3a] 7.11.1절 "HLT Instruction"을 참고하라.
		 */
		// clang-format off
		asm volatile ("sti; hlt" : : : "memory");
		// clang-format on
	}
}

/*
 * 커널 스레드의 기반으로 사용되는 함수.
 */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	/* 스케줄러는 인터럽트가 꺼진 상태에서 실행된다. */
	intr_enable ();
	/* 스레드 함수를 실행한다. */
	function (aux);
	/* function()이 반환하면 스레드를 종료한다. */
	thread_exit ();
}

/*
 * T를 NAME이라는 이름의 blocked 스레드로 기본 초기화한다.
 */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->base_priority = priority;
	t->priority = priority;
	t->nice = 0;
	t->recent_cpu = 0;
	t->magic = THREAD_MAGIC;

	/* phase 3: priority donation 추가 구현 변수 초기화 */
	list_init (&t->donation_list);
	t->waiting_lock = NULL;

#ifdef USERPROG
	t->running_file = NULL;                // 실행 중인 파일은 load 성공 시 보관
	t->fd_table = NULL;                    // fd 테이블은 프로세스 로드 시점에 할당
	t->next_fd = 2;                        // 0, 1은 stdin/stdout 예약
	list_init (&t->child_status_list);     // 자식 상태 레코드 리스트 초기화
	t->self_status = NULL;                 // 아직 연결된 child_status 없음
#endif

#ifdef VM
	t->user_rsp = 0;
	t->stack_bottom = NULL;
#endif
}

/*
 * 다음에 스케줄될 스레드를 선택해 반환한다.
 * run queue가 비어 있지 않다면 그 안에서 스레드를 반환해야 한다.
 * (현재 실행 중인 스레드가 계속 실행 가능하다면 그 스레드도 run queue 안에
 * 있다.) run queue가 비어 있으면 idle_thread를 반환한다.
 */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/*
 * iretq를 사용해 스레드를 시작한다.
 */
void
do_iret (struct intr_frame *tf) {
	// clang-format off
	__asm __volatile ("movq %0, %%rsp\n"
	                  "movq 0(%%rsp),%%r15\n"
	                  "movq 8(%%rsp),%%r14\n"
	                  "movq 16(%%rsp),%%r13\n"
	                  "movq 24(%%rsp),%%r12\n"
	                  "movq 32(%%rsp),%%r11\n"
	                  "movq 40(%%rsp),%%r10\n"
	                  "movq 48(%%rsp),%%r9\n"
	                  "movq 56(%%rsp),%%r8\n"
	                  "movq 64(%%rsp),%%rsi\n"
	                  "movq 72(%%rsp),%%rdi\n"
	                  "movq 80(%%rsp),%%rbp\n"
	                  "movq 88(%%rsp),%%rdx\n"
	                  "movq 96(%%rsp),%%rcx\n"
	                  "movq 104(%%rsp),%%rbx\n"
	                  "movq 112(%%rsp),%%rax\n"
	                  "addq $120,%%rsp\n"
	                  "movw 8(%%rsp),%%ds\n"
	                  "movw (%%rsp),%%es\n"
	                  "addq $32, %%rsp\n"
	                  "iretq"
	                  :
	                  : "g"((uint64_t) tf)
	                  : "memory");
	// clang-format on
}

/*
 * 새 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고,
 * 이전 스레드가 dying 상태라면 그것을 파괴한다.
 *
 * 이 함수가 호출되는 시점에는 thread PREV에서 막 전환된 직후이며,
 * 새 스레드는 이미 실행 중이고 인터럽트는 여전히 비활성화된 상태다.
 *
 * 스레드 전환이 완료되기 전까지는 printf()를 호출하는 것이 안전하지 않다.
 * 실제로는 printf()를 함수의 맨 끝에만 추가해야 한다는 뜻이다.
 */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/*
	 * 핵심 전환 로직이다.
	 * 먼저 전체 실행 문맥을 intr_frame에 복원한 뒤,
	 * do_iret를 호출해 다음 스레드로 전환한다.
	 * 전환이 끝날 때까지는 여기서 어떤 스택도 사용해서는 안 된다는 점에
	 * 유의하라.
	 */
	// clang-format off
	__asm __volatile (
	        /* 사용할 레지스터들을 저장한다. */
	        "push %%rax\n"
	        "push %%rbx\n"
	        "push %%rcx\n"
	        /* 입력값을 한 번만 가져온다. */
	        "movq %0, %%rax\n"
	        "movq %1, %%rcx\n"
	        "movq %%r15, 0(%%rax)\n"
	        "movq %%r14, 8(%%rax)\n"
	        "movq %%r13, 16(%%rax)\n"
	        "movq %%r12, 24(%%rax)\n"
	        "movq %%r11, 32(%%rax)\n"
	        "movq %%r10, 40(%%rax)\n"
	        "movq %%r9, 48(%%rax)\n"
	        "movq %%r8, 56(%%rax)\n"
	        "movq %%rsi, 64(%%rax)\n"
	        "movq %%rdi, 72(%%rax)\n"
	        "movq %%rbp, 80(%%rax)\n"
	        "movq %%rdx, 88(%%rax)\n"
	        "pop %%rbx\n"
	        "movq %%rbx, 96(%%rax)\n"
	        "pop %%rbx\n"
	        "movq %%rbx, 104(%%rax)\n"
	        "pop %%rbx\n"
	        "movq %%rbx, 112(%%rax)\n"
	        "addq $120, %%rax\n"
	        "movw %%es, (%%rax)\n"
	        "movw %%ds, 8(%%rax)\n"
	        "addq $32, %%rax\n"
	        "call __next\n"
	        "__next:\n"
	        "pop %%rbx\n"
	        "addq $(out_iret -  __next), %%rbx\n"
	        "movq %%rbx, 0(%%rax)\n"
	        "movw %%cs, 8(%%rax)\n"
	        "pushfq\n"
	        "popq %%rbx\n"
	        "mov %%rbx, 16(%%rax)\n"
	        "mov %%rsp, 24(%%rax)\n"
	        "movw %%ss, 32(%%rax)\n"
	        "mov %%rcx, %%rdi\n"
	        "call do_iret\n"
	        "out_iret:\n"
	        :
	        : "g"(tf_cur), "g"(tf)
	        : "memory");
	// clang-format on
}

/*
 * 새 프로세스를 스케줄한다. 진입 시점에는 인터럽트가 꺼져 있어야 한다.
 * 이 함수는 현재 스레드의 상태를 status로 바꾼 뒤,
 * 다른 스레드를 찾아 그쪽으로 전환한다.
 * schedule() 안에서 printf()를 호출하는 것은 안전하지 않다.
 */
static void
do_schedule (int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current ()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim = list_entry (list_pop_front (&destruction_req),
		                                    struct thread, elem);
		palloc_free_page (victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/*
	 * 다음 스레드를 running 상태로 표시한다.
	 */
	next->status = THREAD_RUNNING;

	/*
	 * 새 time slice를 시작한다.
	 */
	thread_ticks = 0;

#ifdef USERPROG
	/*
	 * 새 주소 공간을 활성화한다.
	 */
	process_activate (next);
#endif

	if (curr != next) {
		/*
		 * 전환 전 스레드가 dying 상태라면 그 struct thread를 파괴한다.
		 * 단, thread_exit()가 자기 발밑을 걷어차지 않도록 이 일은 늦게
		 * 일어나야 한다. 현재 그 페이지는 스택에서 사용 중이므로, 여기서는
		 * 해제 요청만 큐에 넣는다. 실제 파괴 로직은 schedule() 시작 부분에서
		 * 호출된다.
		 */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 현재 실행 중인 스레드의 정보를 먼저 저장한 뒤 전환한다. */
		thread_launch (next);
	}
}

/*
 * 새 스레드에 사용할 tid를 반환한다.
 */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

static int
fp_from_int (int n) {
	return n * FP_FACTOR;
}

static int
fp_to_int_zero (int x) {
	return x / FP_FACTOR;
}

static int
fp_to_int_nearest (int x) {
	return x >= 0 ? (x + FP_FACTOR / 2) / FP_FACTOR
	              : (x - FP_FACTOR / 2) / FP_FACTOR;
}

static int
fp_add (int x, int y) {
	return x + y;
}

static int
fp_add_int (int x, int n) {
	return x + fp_from_int (n);
}

static int
fp_mul (int x, int y) {
	return (int) (((int64_t) x) * y / FP_FACTOR);
}

static int
fp_mul_int (int x, int n) {
	return x * n;
}

static int
fp_div (int x, int y) {
	return (int) (((int64_t) x) * FP_FACTOR / y);
}

static int
fp_div_int (int x, int n) {
	return x / n;
}

static void
mlfqs_update_load_avg (void) {
	int ready_threads = (int) list_size (&ready_list);
	if (thread_current () != idle_thread)
		ready_threads++;

	load_avg = fp_add (fp_mul (fp_div_int (fp_from_int (59), 60), load_avg),
	                   fp_mul_int (fp_div_int (fp_from_int (1), 60),
	                               ready_threads));
}

static void
mlfqs_update_recent_cpu (struct thread *t) {
	if (t == idle_thread)
		return;
	int twice_load = fp_mul_int (load_avg, 2);
	int coefficient = fp_div (twice_load, fp_add_int (twice_load, 1));
	t->recent_cpu = fp_add_int (fp_mul (coefficient, t->recent_cpu), t->nice);
}

static void
mlfqs_update_priority (struct thread *t) {
	if (t == idle_thread)
		return;
	int priority = PRI_MAX - fp_to_int_zero (fp_div_int (t->recent_cpu, 4))
	               - t->nice * 2;
	if (priority > PRI_MAX)
		priority = PRI_MAX;
	if (priority < PRI_MIN)
		priority = PRI_MIN;
	t->priority = priority;
	t->base_priority = priority;
}

static void
mlfqs_update_all_recent_cpu (void) {
	for (struct list_elem *e = list_begin (&all_list);
	     e != list_end (&all_list); e = list_next (e))
		mlfqs_update_recent_cpu (list_entry (e, struct thread, all_elem));
}

static void
mlfqs_update_all_priorities (void) {
	for (struct list_elem *e = list_begin (&all_list);
	     e != list_end (&all_list); e = list_next (e))
		mlfqs_update_priority (list_entry (e, struct thread, all_elem));
}

/* phase 1: alarm clock 해결 함수 구현부 */
/* 스레드를 sleep_list에 넣기 */
void
thread_sleep (int64_t wake_ticks) {
	/* 현재 cpu에 running하는 스레드 가져오기 */
	struct thread *cur = thread_current ();
	/* interrupt를 꺼놓고, 상태를 변수에 저장하기*/
	enum intr_level old_level = intr_disable ();
	/* 스레드의 wake_tick 설정 */
	cur->wake_tick = wake_ticks;
	/* sleep_list에 특정 속성을 기준으로 순차적으로 넣기 */
	list_insert_ordered (&sleep_list, &cur->elem, cmp_thread_ticks, NULL);
	/* 스케줄링 후보에서 빠짐: ready_list에 들어가지 않음 */
	thread_block ();
	/* 인터럽트 키기? */
	intr_set_level (old_level);
}

/* wake_tick이 작은 순으로 list에 삽입하는 조건 구현 */
bool
cmp_thread_ticks (const struct list_elem *a, const struct list_elem *b,
                  void *aux) {
	struct thread *st_a = list_entry (a, struct thread, elem);
	struct thread *st_b = list_entry (b, struct thread, elem);
	return st_a->wake_tick < st_b->wake_tick;
}

/* sleep_list를 매 틱마다 front를 확인 => 조건 만족 시 unblock하기.
   timer_interrupt에서 호출되므로 이미 인터럽트가 꺼져있다. */
void
thread_awake (int64_t global_ticks) {
	while (!list_empty (&sleep_list)) {
		struct list_elem *e = list_front (&sleep_list);
		struct thread *t = list_entry (e, struct thread, elem);

		if (t->wake_tick > global_ticks)
			break;

		list_pop_front (&sleep_list);
		thread_unblock (t);
	}
}

/* phase 2: priority scheduling 함수 구현부 */

/* ready_list의 최상위 스레드보다 현재 스레드가 낮으면 양보한다. */
void
check_preemption (void) {
	if (list_empty (&ready_list))
		return;

	struct thread *front =
	        list_entry (list_begin (&ready_list), struct thread, elem);
	if (thread_current ()->priority < front->priority) {
		if (intr_context ())
			intr_yield_on_return ();
		else
			thread_yield ();
	}
}

/* donation_list에서 최대 우선순위를 구해 effective priority를 갱신한다. */
void
refresh_priority (struct thread *t) {
	t->priority = t->base_priority;

	if (!list_empty (&t->donation_list)) {
		list_sort (&t->donation_list, thread_priority_donation_elem, NULL);
		struct thread *top = list_entry (list_front (&t->donation_list),
		                                 struct thread, donation_elem);
		if (top->priority > t->priority)
			t->priority = top->priority;
	}
}

/* list_insert_ordered용 비교 함수: priority 내림차순 */
bool
thread_priority (const struct list_elem *a, const struct list_elem *b,
                 void *aux UNUSED) {
	return list_entry (a, struct thread, elem)->priority >
	       list_entry (b, struct thread, elem)->priority;
}
