# Vega

Vega는 **인터페이스 구현자**이며 하나의 **aup 라이브러리**다(`use vega`). 사용자가 보는 인터페이스의
규격과 고도화된 명세를 기반으로 TUI, CLI, GUI, NUI를 쉽게 만들 수 있게 한다.

상태: 첫 컴포넌트(목록·표·포커스·톤)와 표면 셋(TUI·CLI·NUI 문장)이 동작한다.
[설계 001](docs/design/001-first-component.md).

```sh
make                                          # build/vega (옆의 ../pure 체크아웃을 링크)
AUP_PATH=lib ../aup/build/aup build examples/todo.aup -o todo.pmod
build/vega tui todo.pmod --store ~/.todo      # 터미널. 글자 입력 후 Enter, Tab으로 키 모드, Ctrl-C로 끝.
                                              # --store가 있으면 목록이 다음 실행에도 남는다(어느 표면이든)
printf 'text 6d696c6b\nkey Space\nshow\n' | build/vega cli todo.pmod
printf 'text 6d696c6b\nkey Space\nshow\n' | build/vega say todo.pmod
build/vega web todo.pmod --watch              # 브라우저(GUI). --watch: 모듈이 다시 빌드되면 상태를 이어 바꾼다
                                              # (옆에서 AUP_PATH=lib aup watch examples/todo.aup -o todo.pmod)
make check                                    # 참조 모델 대조, 프레임 검사, 대화형 pty 검사, 견고성
```

사람과 프로그램이 같은 앱을 쓴다. `examples/board.aup`는 칸반 보드다. 사람은 Vega 표면으로, 프로그램은 같은 모듈과
저장소를 서빙하는 Pure의 `pure-serve`로 쓴다(라우트 코드가 없다. 보기가 GET, 입력이 POST다).

```sh
AUP_PATH=lib ../aup/build/aup build examples/board.aup -o board.pmod
build/vega tui board.pmod --store ~/.board           # 사람: 제목을 쓰고 Enter, Tab 뒤 ←→로 칸을 옮긴다
../pure/prototypes/connections/build/pure-serve --module board.pmod --store ~/.board --threads 2 &
curl -s localhost:8767/views/counts                  # 1 to do, 1 doing, 0 done
curl -s -X POST -H 'Content-Type: application/json' -d '{"title": "ship it", "lane": 2}' localhost:8767/inputs/post
curl -s 'localhost:8767/views/cards_list?from=0&count=20'
```

한 저장소는 한 번에 한 호스트가 연다(상태의 소유자는 하나다).

## 위치

| 층 | 역할 | 비유 |
|---|---|---|
| [Pure](/srv/git/pure.git) | 런타임. 실행, 메모리와 소유권, 영속 상태, 통신 | JVM |
| [aup](/srv/git/aup.git) | Pure가 실행하는 앱 정의를 작성·검사·빌드 | Java·javac |
| **Vega** | aup 라이브러리. 데이터 표현과 인터페이스 명세로 TUI/CLI/GUI/NUI를 구현 | JavaFX |

aup는 일반적인 투영과 모듈만 제공하고, 목록·표·커서·톤 같은 인터페이스 어휘는 Vega가 제공한다
([aup 002](/srv/git/aup.git): `docs/design/002-dsl-direction.md`). 그래서 aup v0을 먼저 만든다.

## 다루는 것

- **데이터 표현.** 커서링(현재 위치·선택·이동), 컬러링(의미에 따른 색·강조), 리스트, 테이블부터 시작한다.
- **명세에서 표면으로.** 인터페이스를 특정 표면의 위젯이 아니라 명세로 기술하고, 각 표면이 그것을 구현한다.
- **표면.** CLI(비대화형 출력·파이프), TUI(터미널), GUI(창), NUI(음성·대화·제스처 같은 자연 인터페이스).

## 원칙

- **최소 공통 기능에 가두지 않는다.** 표면마다 지원하는 기능을 명시하고, 지원하지 않는 기능을
  조용히 다른 의미로 바꾸지 않는다. 명세는 공통 의미와 표면별 고도화를 함께 담는다.
- **데이터를 표면마다 다시 만들지 않는다.** 표시할 값은 Pure의 런타임 상태(영속 트리·draft)에서
  복사 없이 빌려 온다. 큰 표나 문서를 표면별 모델로 복제하지 않는다.
- **갈라파고스를 만들지 않는다.** ANSI 터미널, 셸 파이프, 플랫폼 접근성 API 같은 기존 규격과
  호환하는 접점을 유지한다.
- **검증 기기는 로컬 Raspberry Pi 5**(Linux AArch64)다.

## 열린 질문

- NUI의 다음 범위. 지금은 화면 낭독 같은 문장이다. 음성 합성·대화형 입력·제스처 중 무엇이 먼저인지.
- 표면별 고도화(예: TUI의 셀 단위 갱신, GUI의 부드러운 스크롤)를 명세의 어디에 둘지. 가상 스크롤은 이미
  런타임의 성질이다(보이는 칸만 계산).
