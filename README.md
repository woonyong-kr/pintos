# PintOS 가상 메모리와 Copy-on-Write

## 무엇을 푸는가

- KAIST PintOS 교육용 커널에서 페이지, 프레임, swap의 생명주기를 구현합니다.
- `fork` 직후 메모리를 복제하지 않고 실제 쓰기 시점까지 복사를 미루는 Copy-on-Write를 다룹니다.
- 팀 구현이며 Git 이력 기준 전체 261개 커밋 중 본인 커밋은 194개입니다.

## 어디를 보면 되는가

| 경로 | 이 파일이 답하는 질문 |
| --- | --- |
| [`pintos/include/vm/vm.h`](pintos/include/vm/vm.h) | 페이지와 프레임은 어떤 메타데이터로 연결됩니까? |
| [`pintos/vm/vm.c`](pintos/vm/vm.c) | page fault가 COW 판정과 프레임 재매핑으로 어떻게 이어집니까? |
| [`pintos/vm/anon.c`](pintos/vm/anon.c) | anonymous page와 공유 swap slot의 참조 수를 어떻게 관리합니까? |
| [`pintos/vm/file.c`](pintos/vm/file.c) | file-backed page의 swap-out과 destroy는 어떤 경로를 탑니까? |
| [`pintos/userprog/exception.c`](pintos/userprog/exception.c) | CPU page fault가 VM 계층으로 어디에서 전달됩니까? |
| [`pintos/tests/vm`](pintos/tests/vm) | lazy loading, stack growth, mmap, swap을 어떤 시나리오로 검증합니까? |

## 설계 판단

### `ref_count`와 owner list를 분리하는 이유

`ref_count`는 한 프레임을 **몇 개의 페이지가 보고 있는지** 답하지만, **어떤 페이지와 페이지 테이블이 보고 있는지**는 답하지 못합니다. 공유 프레임을 회수할 때는 모든 소유 페이지의 `frame` 연결과 각 프로세스의 PML4 매핑을 지워야 하므로 숫자만으로는 충분하지 않습니다. 따라서 공유 수명 판단은 `ref_count`, 실제 역참조 대상 식별은 `(page, thread)` owner list가 담당하도록 책임을 분리합니다.

현재 커밋된 코드에는 프레임 `ref_count`가 있으며, 다중 owner list를 이용한 회수 보완은 아직 커밋되지 않은 작업 트리 변경입니다. 이 README에서는 해당 보완을 완료 기능이나 개인 단독 구현으로 표시하지 않습니다.

### swap slot에도 참조 계수를 두는 이유

COW 대상 페이지가 메모리에서 내려가면 부모와 자식이 같은 swap slot을 공유할 수 있습니다. 한쪽이 먼저 swap-in하거나 종료되어도 다른 쪽은 그 slot을 계속 읽어야 하므로 첫 접근에서 bitmap을 해제하면 안 됩니다. [`pintos/vm/anon.c`](pintos/vm/anon.c)는 별도의 `swap_ref_count`를 두고 참조가 0이 되는 시점에만 slot을 반환합니다.

## 어떻게 확인하나

x86-64 Linux와 QEMU가 준비된 환경에서 다음 명령으로 VM 전체 테스트를 실행합니다.

```bash
make -C pintos/vm check
```

이번 macOS ARM 환경에서는 빌드 중 `clang: error: unsupported option '-mno-sse' for target 'arm64-apple-darwin'`가 발생하여 테스트가 시작되지 않았습니다. 과제 제출 당시 기록은 `141/141`이지만 이번 재실행 결과가 아니며, 저장소에는 해당 결과의 원문 산출물이 포함되어 있지 않습니다.

## 한계

- KAIST PintOS 위에서 수행한 교육 프로젝트이며 상용 OS 개발 경험이 아닙니다.
- 빌드와 테스트는 x86-64 Linux 도구 체인을 전제로 합니다.
- 다중 owner list 기반 프레임 회수 보완은 현재 작업 트리에만 있으며 이 README 커밋의 검증 대상에 포함하지 않습니다.
