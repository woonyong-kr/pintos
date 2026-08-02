# SW_AI-W09 Pintos

> 크래프톤 정글 12기 팀 프로젝트의 개인 보존용 미러다. 원본은 [Jungle-12-303/wk11_7](https://github.com/Jungle-12-303/wk11_7)이며, 개인 기여는 커밋 저자(`woonyong.kr@gmail.com`)와 파일별 `git blame`으로 확인할 수 있다.

KAIST Pintos 기반으로 Project 1~4를 진행하는 팀 저장소입니다.  
현재 저장소는 `Project 2: User Programs`의 `fork`, `exec`, `wait`, `exit`, 파일 디스크립터 흐름을 중심으로 작업 중입니다.

## 프로젝트 개요

- 기준 코드: KAIST Pintos 64-bit
- 주요 작업 영역: `pintos/threads`, `pintos/userprog`, `pintos/vm`
- 문서화 기준: `docs/convention`
- 작업 브랜치: `dev`, `woonyong`

## 현재 상태

- `create`, `open`, `close`, `write`, `exit` 시스템 콜 기본 흐름이 들어가 있습니다.
- `fork`는 `child_status` 기반 부모-자식 메타데이터 구조와 시스템 콜 진입 경로까지 연결되어 있습니다.
- `wait`, `exec`, `exit` 전체 생명주기 완성은 아직 진행 중입니다.
- 취소된 PR의 비중복 차이는 [docs/merge-recovery](docs/merge-recovery/README.md)에 복구 아카이브로 보관합니다.

## 빠른 이동

- 변경 이력: [CHANGELOG.md](CHANGELOG.md)
- 프로세스 테스트 정리: [pintos-project2-process-tests.md](pintos-project2-process-tests.md)
- 구현 가이드: [docs/pintos/08-implementation-guide.md](docs/pintos/08-implementation-guide.md)
- 팀 전략: [docs/pintos/09-team-strategy.md](docs/pintos/09-team-strategy.md)
- 코드/문서 컨벤션: [docs/convention/README.md](docs/convention/README.md)

## 디렉터리 구조

```text
SW_AI-W09-pintos/
├── README.md
├── CHANGELOG.md
├── pintos-project2-process-tests.md
├── docs/
│   ├── convention/
│   ├── merge-recovery/
│   └── pintos/
├── pintos/
│   ├── threads/
│   ├── userprog/
│   ├── vm/
│   └── README.md
└── scripts/
```

## 개발 환경

이 저장소는 `x86-64 Linux` 기준 Pintos 빌드 환경을 전제로 합니다.

- 권장 환경: VS Code Dev Container 또는 Ubuntu x86-64
- 로컬 macOS `arm64`에서 바로 `make`를 실행하면 `-mno-sse` 옵션 때문에 실패할 수 있습니다.
- 따라서 실제 빌드/테스트는 컨테이너 또는 x86-64 Linux 환경에서 진행하는 것을 권장합니다.

예시:

```bash
make -C pintos/threads
make -C pintos/userprog
```

## 작업 원칙

- 구현 변경은 가능하면 `dev`와 `woonyong`을 같은 기준점으로 유지합니다.
- 미완성 코드라도 작업 추적이 필요하면 `CHANGELOG.md`와 관련 문서에 남깁니다.
- 취소되거나 보류된 코드 조각은 삭제만 하지 않고 복구 가능한 형태로 남깁니다.

## 변경로그 작성 규칙

변경로그는 [CHANGELOG.md](CHANGELOG.md)에 아래 형식으로 누적합니다.

```md
[버전] - YYYY-MM-DD
Added
- 새 기능

Changed
- 동작 변경

Fixed
- 버그 수정
```

카테고리는 `Added`, `Changed`, `Fixed`, `Removed`, `Security`, `Docs`, `Refactored`를 기본으로 사용합니다.

## 참고

- Pintos 원문 안내: [pintos/README.md](pintos/README.md)
- KAIST Pintos 문서: [https://casys-kaist.github.io/pintos-kaist/](https://casys-kaist.github.io/pintos-kaist/)
