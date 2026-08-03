/*
 * 이 파일은 교육용 운영체제 Nachos의 소스 코드에서 파생되었다.
 * Nachos의 저작권 고지는 아래에 전문이 재수록되어 있다.
 */

/*
 * Copyright (c) 1992-1996 The Regents of the University of California.
 * All rights reserved.
 *
 * 이 소프트웨어와 그 문서를 어떤 목적이든, 사용료 없이, 그리고 별도 서면 계약 없이
 * 사용, 복사, 수정, 배포할 수 있는 권한을 부여한다. 단, 위의 저작권 고지와 아래의
 * 두 문단이 이 소프트웨어의 모든 사본에 포함되어야 한다.
 *
 * 어떤 경우에도 University of California는 이 소프트웨어와 문서의 사용으로 인해
 * 발생하는 직접적, 간접적, 특수, 부수적 또는 결과적 손해에 대해 책임지지 않는다.
 * 설령 그러한 손해 가능성을 사전에 통지받았더라도 마찬가지다.
 *
 * University of California는 명시적이든 묵시적이든 어떠한 보증도 하지 않으며,
 * 여기에는 상품성 및 특정 목적 적합성에 대한 묵시적 보증도 포함되지만 이에
 * 한정되지 않는다. 이 소프트웨어는 "있는 그대로" 제공되며, University of
 * California는 유지보수, 지원, 업데이트, 개선, 수정 제공 의무를 지지 않는다.
 */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 보조 함수 선언부. */
static bool cmp_sema_priority(const struct list_elem *a, const struct list_elem *b, void *aux);
bool thread_priority_donation_elem(const struct list_elem *a,
                                   const struct list_elem *b, void *aux);

/*
 * 세마포어 SEMA를 VALUE로 초기화한다.
 * 세마포어는 음이 아닌 정수 값과, 그것을 조작하는 두 개의 원자적 연산으로 구성된다.
 *
 * - down 또는 "P": 값이 양수가 될 때까지 기다린 뒤 그 값을 감소시킨다.
 *
 * - up 또는 "V": 값을 증가시키고(대기 중인 스레드가 있다면 하나를 깨운다).
 */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/*
 * 세마포어에 대한 down 또는 "P" 연산.
 * SEMA의 값이 양수가 될 때까지 기다렸다가 원자적으로 감소시킨다.
 *
 * 이 함수는 sleep할 수 있으므로 인터럽트 핸들러 안에서 호출하면 안 된다.
 * 인터럽트를 끈 상태에서 호출될 수는 있지만, 실제로 sleep하게 되면
 * 다음에 스케줄된 스레드가 인터럽트를 다시 켤 가능성이 크다.
 * 이것이 sema_down 함수다.
 */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		// list_push_back (&sema->waiters, &thread_current ()->elem);
		/* waiters의 순서를 보장하기 위해 변경 */
		list_insert_ordered(&sema->waiters, &thread_current()->elem, thread_priority, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/*
 * 세마포어에 대한 down 또는 "P" 연산이지만,
 * 세마포어 값이 이미 0이 아닐 때만 수행한다.
 * 세마포어를 감소시켰다면 true를, 아니면 false를 반환한다.
 *
 * 이 함수는 인터럽트 핸들러에서 호출될 수 있다.
 */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/*
 * 세마포어에 대한 up 또는 "V" 연산.
 * SEMA의 값을 증가시키고, SEMA를 기다리는 스레드가 있다면 그중 하나를 깨운다.
 *
 * 이 함수는 인터럽트 핸들러에서 호출될 수 있다.
 */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)){
		list_sort(&sema->waiters, thread_priority, NULL);
		thread_unblock (list_entry (list_pop_front (&sema->waiters),
					struct thread, elem));
	}

	sema->value++;

	intr_set_level (old_level);

	/* 즉시 선점 반영하기 */
	check_preemption();
}

static void sema_test_helper (void *sema_);

/*
 * 세마포어 self-test.
 * 두 스레드 사이에서 제어가 "핑퐁"처럼 오가도록 만든다.
 * 무슨 일이 일어나는지 보려면 printf() 호출을 넣어 보라.
 */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/*
 * sema_self_test()에서 사용하는 스레드 함수.
 */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/*
 * LOCK을 초기화한다.
 * 락은 어느 시점이든 최대 하나의 스레드만 가질 수 있다.
 * 우리의 락은 "재귀적(recursive)"이지 않다.
 * 즉, 현재 락을 들고 있는 스레드가 같은 락을 다시 얻으려 하면 오류다.
 *
 * 락은 초기값이 1인 세마포어의 특수화다.
 * 락과 그러한 세마포어의 차이는 두 가지다.
 * 첫째, 세마포어는 값이 1보다 클 수 있지만 락은 한 번에 하나의 스레드만 소유할 수 있다.
 * 둘째, 세마포어는 소유자가 없으므로 한 스레드가 "down"하고 다른 스레드가 "up"할 수 있지만,
 * 락에서는 같은 스레드가 acquire와 release를 모두 해야 한다.
 * 이런 제약이 부담스럽다면 락 대신 세마포어를 사용해야 한다는 신호다.
 */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/*
 * LOCK을 획득한다.
 * 필요하다면 사용 가능해질 때까지 잠들어 기다린다.
 * 현재 스레드는 이미 이 락을 들고 있으면 안 된다.
 *
 * 이 함수는 sleep할 수 있으므로 인터럽트 핸들러 안에서 호출하면 안 된다.
 * 인터럽트를 끈 상태에서 호출할 수는 있지만, sleep이 필요하면 인터럽트는
 * 다시 켜지게 된다.
 */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));
	/* 여기서 priority donation 해야 할 듯? */
	struct thread *curr = thread_current();

	/* 나(curr)를 lock하는 holder 스레드가 존재한다면? */
	if(!thread_mlfqs && lock->holder != NULL){
		struct thread *holder = lock->holder;
		int idx = 0;
		
		/* holder의 donation_list에 추가하기: 내림차순 */
		list_insert_ordered(&holder->donation_list, &curr->donation_elem,
		                    thread_priority_donation_elem, NULL);
		/* 내가 누구에 의해 lock 되었는지 명시하기 */
		curr->waiting_lock = lock;
		
		/* 만약 lock 소유 스레드의 priority가 낮다면? => 기부 */
		if(holder->priority < curr->priority){
			holder->priority = curr->priority;
		}

		/* 일단 임시로 여기에 중첩 기부를 넣기로... */
		while(idx < 7){
			struct lock *next_lock = holder->waiting_lock;
			if(next_lock == NULL) break;
			holder = next_lock->holder;
			
			if(holder == NULL) break;
			/* 여기서 holder는 상위의 thread */
			if( holder->priority < curr->priority){
				holder->priority = curr->priority;
			}

			idx++;
		}
	}

	/* 단순히 sema_val을 낮출 뿐만 아니라, lock 획득을 위한 대기함(wait에 넣기) */
	sema_down (&lock->semaphore);
	/* 여기선 락을 획득했으니까, waiting_lock을 null 처리 */
	curr->waiting_lock = NULL;
	lock->holder = thread_current ();
}

/*
 * LOCK 획득을 시도하고 성공하면 true, 실패하면 false를 반환한다.
 * 현재 스레드는 이미 이 락을 들고 있으면 안 된다.
 *
 * 이 함수는 sleep하지 않으므로 인터럽트 핸들러 안에서 호출될 수 있다.
 */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/*
 * LOCK을 해제한다.
 * 이 락은 현재 스레드가 소유하고 있어야 한다.
 * 이것이 lock_release 함수다.
 *
 * 인터럽트 핸들러는 락을 획득할 수 없으므로,
 * 인터럽트 핸들러 안에서 락을 해제하려는 것은 의미가 없다.
 */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	/* MLFQS에서는 우선순위 기부를 사용하지 않는다. */
	if (thread_mlfqs) {
		lock->holder = NULL;
		sema_up (&lock->semaphore);
		return;
	}

	/* priority를 경우에 따른 하향을 하는 로직 구현 */
	/* t = lock 잃는 스레드 */
	struct thread *t = thread_current();

	/* holder 스레드의 donation_list을 뺀다 => 정확히는 .. 이 lock에 의해 대기탄 스레드들만 모두 뺸다 */
	struct list *d_list = &t->donation_list;
	struct list_elem *d_e = list_begin(d_list);
	
	while(d_e != list_end(d_list)){
		struct thread *d_t = list_entry(d_e, struct thread, donation_elem);
		/* 순회 꼬이지 않도록 미리미리 다음 것 확보하기 */
		struct list_elem *next = list_next(d_e);

		/* 이 스레드가 어떤 lock에 의해 대기타는지 확인 => 같은 lock이면 제거 */
		if(d_t->waiting_lock == lock){
			list_remove(d_e);
		}

		d_e = next;
	}

	/* donation_list를 재정렬하고 effective priority를 갱신한다. */
	refresh_priority(t);

	lock->holder = NULL;
	sema_up (&lock->semaphore);

}

/*
 * 현재 스레드가 LOCK을 들고 있으면 true를, 아니면 false를 반환한다.
 * (다른 어떤 스레드가 락을 들고 있는지 검사하는 것은 race 상태가 될 수 있다.)
 */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/*
 * 리스트 안의 세마포어 하나.
 */
struct semaphore_elem {
	/*
	 * 리스트 원소.
	 */
	struct list_elem elem;
	/*
	 * 이 세마포어.
	 */
	struct semaphore semaphore;
};

/*
 * 조건 변수 COND를 초기화한다.
 * 조건 변수는 한쪽 코드가 어떤 조건을 신호로 보내고,
 * 협력하는 다른 코드가 그 신호를 받아 동작하도록 해 준다.
 */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/*
 * LOCK을 원자적으로 해제하고, 다른 코드가 COND를 신호로 보낼 때까지 기다린다.
 * COND가 신호된 뒤에는 반환 전에 LOCK을 다시 획득한다.
 * 이 함수를 호출하기 전에는 반드시 LOCK을 들고 있어야 한다.
 *
 * 이 함수가 구현하는 모니터는 "Hoare" 스타일이 아니라 "Mesa" 스타일이다.
 * 즉, 신호를 보내는 것과 받는 것은 원자적 연산이 아니다.
 * 따라서 일반적으로 호출자는 wait이 끝난 뒤 조건을 다시 검사하고,
 * 필요하면 다시 기다려야 한다.
 *
 * 하나의 조건 변수는 오직 하나의 락과만 연관되지만,
 * 하나의 락은 여러 조건 변수와 연관될 수 있다.
 * 즉, 락에서 조건 변수로는 one-to-many 관계다.
 *
 * 이 함수는 sleep할 수 있으므로 인터럽트 핸들러 안에서 호출하면 안 된다.
 * 인터럽트를 끈 상태에서 호출할 수는 있지만, sleep이 필요하면 인터럽트는
 * 다시 켜지게 된다.
 */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	/* signal 시점에 동적 우선순위로 정렬하므로 삽입 순서는 무관 */
	list_push_back (&cond->waiters, &waiter.elem);
	lock_release (lock);
	/* 스레드가 세마포어 리스트에 들어가는 시점(중요!) */
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/*
 * LOCK으로 보호되는 COND를 기다리는 스레드가 있다면,
 * 이 함수는 그중 하나를 깨우도록 신호를 보낸다.
 * 이 함수를 호출하기 전에는 반드시 LOCK을 들고 있어야 한다.
 *
 * 인터럽트 핸들러는 락을 획득할 수 없으므로,
 * 인터럽트 핸들러 안에서 조건 변수에 신호를 보내려는 것은 의미가 없다.
 */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));
	if (!list_empty (&cond->waiters)){
		/* 동적 우선순위 기준으로 재정렬 후 가장 높은 것을 깨운다 */
		list_sort(&cond->waiters, cmp_sema_priority, NULL);
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
	}
}

/*
 * LOCK으로 보호되는 COND를 기다리는 모든 스레드를 깨운다.
 * 이 함수를 호출하기 전에는 반드시 LOCK을 들고 있어야 한다.
 *
 * 인터럽트 핸들러는 락을 획득할 수 없으므로,
 * 인터럽트 핸들러 안에서 조건 변수에 신호를 보내려는 것은 의미가 없다.
 */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}

/* 보조 함수 구현부. */
/* 세마포어 waiter list에서 가장 높은 우선순위의 스레드를 기준으로 비교 */
static bool
cmp_sema_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
	struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

	/* 각 세마포어의 waiter 중 가장 높은 우선순위 스레드를 찾는다 */
	list_sort(&sa->semaphore.waiters, thread_priority, NULL);
	list_sort(&sb->semaphore.waiters, thread_priority, NULL);

	struct thread *ta = list_entry(list_front(&sa->semaphore.waiters),
								   struct thread, elem);
	struct thread *tb = list_entry(list_front(&sb->semaphore.waiters),
								   struct thread, elem);
	return ta->priority > tb->priority;
}

/* donation_elem 기준 priority 구하기 */
bool
thread_priority_donation_elem(const struct list_elem *a,
                              const struct list_elem *b, void *aux UNUSED){
	/* 이번에는 좀 더 간결하게 표현 */
	return list_entry(a, struct thread, donation_elem)->priority >
		   list_entry(b, struct thread, donation_elem)->priority;
}
