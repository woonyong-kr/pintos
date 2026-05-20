# PintOS Project 2 — 포크/프로세스 관련 테스트 분류

> 전체 66개 테스트 중 포크·exec·wait·프로세스 라이프사이클과 관련된 항목만 추출.
> 파일 시스콜 전용 테스트(Create, Open, Close, Read, Write)는 제외.

## 담당 범위 요약

| 카테고리 | 테스트 수 | 필요 syscall |
|----------|-----------|-------------|
| Fork | 6 | fork, wait, exit (+일부 file ops) |
| Exec | 6 | fork, wait, exec |
| Wait | 4 | fork, wait, exit, exec |
| Multi | 2 | fork, wait, exec, open, close |
| ROX (프로세스 관련) | 2 | fork, wait, exec, open, write |
| No-VM | 1 | fork, wait, exit |
| **합계** | **21** | |

---

## Fork (6개)

### fork-once
- 단일 자식을 fork하고 wait한다.
- 필요: fork, wait, exit

### fork-multiple
- fork_and_wait()를 4번 순차 호출한다. 매번 magic 값을 증가시키고 자식이 해당 값으로 exit한다.
- 필요: fork, wait, exit

### fork-recursive
- 재귀적으로 fork한다. magic이 10 이상이면 exit, 아니면 자식이 다시 fork_and_wait를 호출한다.
- 필요: fork, wait, exit

### fork-read
- 부모가 파일을 open한 뒤 fork한다. 자식이 같은 fd로 read/close한다. 부모는 wait 후 해당 fd에 다시 접근하여 파일 핸들 독립성을 검증한다.
- 필요: fork, wait, exit, open, read, close, seek
- 참고: fd_table 복제(file_duplicate)가 올바르게 동작해야 통과

### fork-close
- fork-read와 유사. 자식이 fd를 close한 후, 부모가 같은 fd로 접근할 수 있는지 확인한다.
- 필요: fork, wait, exit, open, close, seek

### fork-boundary
- 프로세스 이름이 페이지 경계를 걸치는 경우의 fork 동작을 테스트한다.
- 필요: fork, wait, exit

---

## Exec (6개)

### exec-once
- exec으로 자식 프로세스를 실행하고 wait한다.
- 필요: fork, wait, exec

### exec-arg
- exec에 인자를 전달하고 자식이 인자를 올바르게 받는지 검증한다.
- 필요: fork, wait, exec

### exec-boundary
- exec할 파일 이름이 페이지 경계를 걸치는 경우를 테스트한다.
- 필요: fork, wait, exec

### exec-missing
- 존재하지 않는 프로그램을 exec하면 -1을 반환해야 한다.
- 필요: exec

### exec-bad-ptr
- 유효하지 않은 포인터를 exec에 전달하면 프로세스가 -1로 종료해야 한다.
- 필요: exec

### exec-read
- 파일을 open/read한 뒤 exec한다. exec 후 이전 메모리가 교체되는지 확인한다.
- 필요: open, read, seek, exec, fork, wait

---

## Wait (4개)

### wait-simple
- fork 후 exec하고, 부모가 wait하여 자식의 종료 코드를 받는다.
- 필요: fork, wait, exit, exec

### wait-twice
- 같은 자식에 wait를 두 번 호출한다. 첫 번째는 종료 코드(81) 반환, 두 번째는 -1 반환.
- 필요: fork, wait, exit, exec
- 참고: list_remove 후 재탐색 실패가 올바르게 동작해야 통과

### wait-killed
- 비정상 종료(exception)하는 자식을 wait한다. -1이 반환되어야 한다.
- 필요: fork, wait, exit, exec
- 참고: exception handler에서 exit(-1) 호출이 필요

### wait-bad-pid
- 존재하지 않는 PID로 wait하면 -1을 반환해야 한다.
- 필요: wait

---

## Multi (2개)

### multi-recurse
- 재귀적으로 자기 자신을 exec한다. depth 인자를 줄여가며 종료 코드를 전파한다.
- 필요: fork, wait, exec

### multi-child-fd
- 부모가 파일을 open한 뒤 fork+exec한다. 자식이 부모의 fd를 close 시도한다. 파일 핸들이 상속되지 않아야 한다.
- 필요: fork, wait, exec, open, close, read

---

## ROX — 프로세스 관련만 (2개)

### rox-child
- 자식 프로세스가 실행 중인 자기 자신의 바이너리에 write를 시도한다. 거부되어야 한다.
- 필요: fork, wait, exec, open, read, write, seek
- 참고: rox-simple은 fork 없이 단일 프로세스에서 테스트하므로 파일 시스콜 담당

### rox-multichild
- rox-child를 재귀적으로 5단계 깊이로 실행한다. 모든 단계에서 write 보호가 유지되어야 한다.
- 필요: fork, wait, exec, open, read, write, seek

---

## No-VM (1개)

### multi-oom
- 메모리가 부족할 때까지 재귀적으로 fork한다. 최소 10회 depth를 달성해야 하고, 10번 반복하여 메모리 누수가 없는지 확인한다.
- 필요: fork, wait, exit, open, close
- 참고: 고아 해방 루프 + fd_table 해제가 완벽해야 통과. 가장 까다로운 테스트.

---

## 구현 의존성 정리

### B1 (wait/exit 동기화) 단독으로 테스트 가능한 항목
- wait-bad-pid: child_list 검색 실패 -> -1 반환만 확인
- exec-missing: exec 실패 시 -1 반환

### B1 + B2 합류 후 테스트 가능한 항목
- fork-once, fork-multiple, fork-recursive, fork-boundary
- wait-simple, wait-twice, wait-killed
- exec-once, exec-arg, exec-boundary, exec-bad-ptr
- multi-recurse

### B1 + B2 + A (파일 시스콜) 합류 후 테스트 가능한 항목
- fork-read, fork-close
- exec-read
- multi-child-fd
- rox-child, rox-multichild
- multi-oom (최종 통합 테스트)
