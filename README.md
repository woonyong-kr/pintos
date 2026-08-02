# PintOS

> 크래프톤 정글 12기 팀 프로젝트의 개인 보존용 미러다. 원본은 [Jungle-12-303/wk11_7](https://github.com/Jungle-12-303/wk11_7)이며, 개인 기여는 커밋 저자(`woonyong.kr@gmail.com`)와 파일별 `git blame`으로 확인할 수 있다.

## 한눈에

| 구분 | 내용 |
|---|---|
| 무엇 | KAIST PintOS(x86-64) 위에 스레드 스케줄링과 가상 메모리를 구현한 교육용 OS 과제 |
| 왜 | 자원을 여럿이 공유할 때 "몇 개가 보는가"와 "누가 보는가"를 커널 코드로 답해 보기 위해 |
| 내 몫 | 팀 구현. 이 미러 main 기준 215 커밋 중 160 이 내 커밋이고, 스왑과 COW 계층을 끝까지 잡았다. 파일 단위 경계는 git blame 으로 확인한다 |
| 스택 | C · x86-64 · QEMU |
| 검증된 사실 | 팀 저장소 기록 기준 141개 테스트 통과. 기록값이며 이 미러에서 재실행하지 않았다 |
| 한계 | 교육용 커널. macOS arm64 에서는 빌드되지 않아 x86-64 컨테이너가 필요하다 |

**같은 사람의 다른 저장소** · 이력서 허브: <https://woonyong-kr.github.io>
[Kyro(k8s-ops)](https://github.com/woonyong-kr/k8s-ops) · [MiniDB](https://github.com/woonyong-kr/minidb) · [PintOS](https://github.com/woonyong-kr/pintos) · [dx_framework](https://github.com/woonyong-kr/dx_framework) · [dx_content_interface](https://github.com/woonyong-kr/dx_content_interface)

## 무엇을 푸는가

- 우선순위대로 실행되는 스레드 스케줄러를 만들고, 락이 우선순위를 뒤집는 문제를 기부(donation)로 풉니다.
- 쓸 때만 메모리를 할당하는 가상 메모리를 만듭니다. demand paging, mmap, stack growth, 익명 페이지 스왑이 과제 범위입니다.
- fork 를 수정 전까지 메모리를 공유하는 방식(Copy-on-Write)으로 바꾸고, 공유된 프레임의 회수 조건을 참조 계수로 관리합니다.

## 어디를 보면 되는가

| 경로 | 이 파일이 답하는 질문 |
| --- | --- |
| [`pintos/threads/`](pintos/threads) | 우선순위 스케줄링과 기부는 어떻게 동작합니까? |
| [`pintos/userprog/process.c`](pintos/userprog/process.c) | fork, exec, wait 에서 부모와 자식의 수명은 어떻게 얽힙니까? |
| [`pintos/vm/vm.c`](pintos/vm/vm.c) | 페이지 폴트 한 번이 COW 복사나 스왑 인으로 어떻게 갈라집니까? |
| [`pintos/include/vm/vm.h`](pintos/include/vm/vm.h) | 프레임이 `ref_count` 와 소유자를 어떻게 들고 있습니까? |
| [`pintos/vm/anon.c`](pintos/vm/anon.c) | 익명 페이지는 스왑 슬롯을 언제 얻고 언제 반납합니까? |
| [`docs/pintos/`](docs/pintos) | 각 단계를 어떤 순서와 판단으로 구현했습니까? |

## 설계 판단

### fork 를 COW 로 바꾼 이유

fork 시점에 모든 페이지를 복사하면 대부분 곧 exec 로 버려집니다. 부모와 자식이 같은 프레임을 읽기 전용으로 공유하다가, 어느 쪽이든 쓰는 순간에만 복사합니다. 프레임의 `ref_count` 가 1 로 떨어지면 복사 없이 다시 쓰기 가능으로 되돌립니다 ([vm.c 의 `vm_handle_cow`](pintos/vm/vm.c)).

### 회수에는 개수와 소유자가 둘 다 필요한 이유

프레임을 스왑으로 내보내려면 그 프레임을 가리키던 페이지 테이블 항목을 지워야 합니다. 참조 계수는 몇 개가 보는지만 말하고, 지울 자리는 소유자를 알아야 찾습니다. 그래서 프레임이 소유자를 함께 들고, 회수 경로가 그 소유자의 페이지 테이블을 정리합니다 ([include/vm/vm.h 의 `struct frame`](pintos/include/vm/vm.h)).

### 커널에 남던 좀비 기록을 회수한 방법

부모가 wait 하지 않고 죽은 자식의 기록이 커널에 남는 문제를 잡아, 부모의 종료 시점에 남은 자식 기록을 걷어 냅니다. 판단 과정은 [docs/pintos/10-orphan-reaping.md](docs/pintos/10-orphan-reaping.md) 에 있습니다.

## 어떻게 확인하나

x86-64 Linux 컨테이너(또는 VS Code Dev Container) 안에서 실행합니다. macOS arm64 에서는 `-mno-sse` 옵션 때문에 빌드되지 않습니다.

```bash
cd pintos/vm
make check          # 가상 메모리 테스트
cd ../threads
make check          # 스레드 테스트
```

통과 수치 141 은 팀 저장소의 기록값입니다. 이 미러에서 재실행해 얻은 값이 아니므로 현재 값과 다를 수 있습니다.

## 더 읽기

- [우선순위가 뒤집힐 때](https://woonyong-kr.github.io/#/posts/priority-inversion)
- [수정 전까지 공유하는 fork](https://woonyong-kr.github.io/#/posts/cow-fork)
- [스왑 슬롯은 언제 반납되는가](https://woonyong-kr.github.io/#/posts/swap-sharing)
- [커널에 남던 기록을 회수하기](https://woonyong-kr.github.io/#/posts/orphan-records)

## 한계

- 교육용 커널입니다. 사용자 수나 운영 성과를 주장하지 않습니다.
- 팀 저장소의 이후 작업은 이 미러에 순차 반영 중입니다. 미러와 팀 저장소의 최신 상태가 다를 수 있습니다.
- 기준 코드(KAIST Pintos)와 테스트 스위트는 과제 제공물입니다. 구현한 것은 그 위의 스케줄러, 시스템 콜, 가상 메모리 계층입니다.

---

## 만든 사람

**최우녕** — AI 애플리케이션을 만듭니다. LLM 의 판단 범위를 계약과
테스트로 고정하고, 만든 것은 골든셋 · 실측 벤치마크로 검증합니다.
게임사 총괄 PD 로 프로젝트 7건을 리딩한 뒤, 기술 결정의 근거를
바닥부터 다시 확인하기 위해 크래프톤 정글에서 OS · DB · 웹 서버를
직접 구현했습니다.

woonyong.kr@gmail.com
