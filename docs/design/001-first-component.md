# 001 — 첫 컴포넌트: 목록·표·포커스·톤, 표면 셋

2026-09-25. 상태: 구현(`src/`, `lib/vega.aup`, `examples/`).

## 결정

Vega의 명세는 **새 언어나 위젯 트리가 아니라 aup 규약**이다. 앱은 aup로 상태와 보기를 선언하고,
Vega는 검증된 Pure 모듈의 보기를 이름과 타입으로 읽어 인터페이스 부품으로 해석한다. 인터페이스용
화면 모델을 따로 만들지 않고, 표면은 Pure에 보이는 칸만 물어 그린다.

| 모듈에 있는 것 | Vega가 읽는 부품 |
|---|---|
| text 보기 `title` | 화면 제목 |
| `label` text 열이 있는 표 보기 | **목록**. 행마다 `label`, 선택적으로 `badge`(text), `detail`(text), `tone`(수) |
| `label` 열이 없는 표 보기 | **표**. `tone`을 뺀 모든 열이 차례로 보인다 |
| 수 보기 `<표 이름>_focus` | 그 목록·표의 포커스 행(0부터) |
| 그 밖의 수·text 보기 | **필드**. 이름과 값 |
| 키 입력과 그 핸들러 | 표면이 보여 주는 키 목록 |
| 첫 text 입력 | 입력 줄(TUI), 스크립트의 `text` 명령 |

톤은 의미다(`lib/vega.aup`: `normal muted accent good warning danger`, `vega.when(조건, 톤)`). 색과 소리는 표면이
정한다. 지원하지 않는 표현은 다른 의미로 바꾸지 않고 드러낸다. 색이 없는 CLI는 톤을 `(danger)`처럼 글자로 적는다.

```text
use vega
view tasks_list = each task in tasks {
  label = task.title
  badge = if task.done { "done" } else if task.urgent { "urgent" } else { "" }
  tone = if task.done { vega.muted } else { vega.when(task.urgent, vega.danger) }
}
view tasks_list_focus = at.position
```

## 표면

| 표면 | 명령 | 하는 일 |
|---|---|---|
| TUI | `vega tui app.pmod` | 대체 화면, 원시 입력. 제목·필드·목록·표·키 목록·입력 줄. 포커스 행을 최소 스크롤로 화면에 두고 **보이는 행의 칸만** 계산한다. 크기 변경(SIGWINCH), frame 핸들러가 있으면 60 Hz |
| CLI | `vega cli app.pmod < script` | 파이프용 평문. 목록은 `> ` 표시와 `[badge]`, 표는 표시 폭으로 정렬한 열. 터미널이고 `NO_COLOR`가 없을 때만 색 |
| NUI | `vega say app.pmod < script` | 문장. 이벤트마다 포커스 항목을 말하고(`Item 2 of 3: danger, milk, urgent.`), 실패하면 `Could not toggle.`, `show`에서 화면 전체 |

TUI 입력: 앱에 text 입력이 있으면 처음에는 글자가 입력 줄로 가고 Enter가 확정한다(IME 확정분과 같은 의미).
Tab이 키 모드로 바꾸면 글자·숫자·Space가 `KeyA`·`Digit0`·`Space` 입력이 된다(앱이 Tab을 쓰면 Tab은 앱의 것).
화살표·Delete·Home 같은 키는 두 모드 모두 앱으로 간다. Ctrl-C나 Ctrl-Q로 끝내고 터미널을 되돌린다.

표시 폭은 Vega가 맡는다. 한글·CJK·그림 이모지는 두 칸, 결합 문자·변이 선택자·ZWJ는 0칸, 제어 문자는
제어 그림(U+2400대)으로 보인다. grapheme 단위 자르기는 아직 없어 ZWJ 이모지 묶음은 근사다.

## 규약 검사 (`vega check`)

`vega check app.pmod`는 모듈이 뜻한 듯하지만 규약에 맞지 않는 곳을 알린다. 표면은 이런 보기를 조용히 다른
부품으로 읽는다. 검사는 그 읽기를 드러내어 오타나 타입 실수가 화면에서 사라지지 않게 한다. 찾으면 종료 코드 1이다.
규칙은 화면을 읽는 코드와 같은 파일(`src/screen.c`)에 있다.

| 찾는 것 | 표면이 대신 하는 일 |
|---|---|
| 수인 `title` | 필드로 보인다 |
| 수인 `label` 열 | 목록이 아니라 표로 보인다 |
| text인 `tone` 열 | 목록은 무시, 표는 열로 보인다 |
| 수인 `badge`·`detail` 열 | 보이지 않는다 |
| 목록의 그 밖의 열 | 보이지 않는다(목록은 label, badge, detail, tone만 보인다) |
| 보일 열이 없는 표 | 제목과 행 수만 보인다 |
| 목록·표가 없는 `X_focus`, text인 `_focus` | 필드로 보이거나 포커스가 없다 |
| 두 번째 text 입력 | 입력할 곳이 없다(첫 번째만 입력 줄을 얻는다) |

```text
$ vega check bad.pmod
bad.pmod: items_list.tone: a tone is a number (vega.normal ... vega.danger); the list ignores it
bad.pmod: missing_focus: names no list or table missing; it shows as a field
```

## 영속

모든 표면은 `--store 경로`를 받는다. 앱의 상태는 Pure가 그 트리 저장소에 두므로, 터미널에서 적은 할 일을 다음 실행의
CLI나 문장 표면이 그대로 이어받는다. Vega에도 앱에도 저장 코드가 없다([Pure 008](/srv/git/pure.git) 상태 영속).
끝날 때 frame 변경까지 저장한다.

## 검증 (`make check`)

- `examples/todo.aup`에 무작위 추가·이동·토글·긴급·삭제 스크립트를 주고, CLI 출력과 NUI 문장이 앱과 Vega 규약의
  Python 참조 모델과 줄 단위로 같다(8시드, 약 8,000줄, 빈 목록에서 실패하는 이벤트 약 200건 포함).
- TUI 프레임 9,600개(40×12, 24×9, 80×30, 16×6)에서 보이는 행 범위와 최소 스크롤, 포커스 행, 마지막 열에서 끝나는 배지,
  말줄임, 행 톤이 모델과 같다. 프레임마다 계산한 칸 수가 `제목 + 필드 + 포커스 2회 + 보이는 행 × 3`과 정확히 같다.
  2,000항목 목록도 한 프레임에 25칸만 계산한다.
- `examples/scores.aup`의 표가 표시 폭으로 정렬되고(한글·결합 문자 포함) 행 톤이 표시된다.
- 가상 터미널에서 대화형 TUI를 실제로 띄워 입력·확정·키 모드·종료·터미널 복원을 확인한다.
- 한 저장소를 CLI와 문장 표면이 번갈아 이어 쓰며(시드마다 7번) 매번 모델과 같은 목록을 보이고, 대화형 세션에서 입력한
  것이 다음 실행에 남는다.
- aup와 Vega의 예제 11개를 세 표면에 무작위 입력으로 돌려 비정상 종료가 없다. sanitizer 빌드로도 같은 검사를 통과한다.
- 규약을 여덟 가지로 어긴 명세에서 `vega check`가 아홉 곳을 모두 정확한 문장으로 알리고, 예제에서는 아무것도 알리지
  않는다.

## 남은 것

- 포커스를 표면에서 바꾸기(GUI 클릭, TUI 마우스). 지금은 앱의 키 핸들러만 포커스를 옮긴다. Pure에 표의 인덱스를
  이벤트로 바꾸는 호스트 API가 필요하다.
- 셀 단위 갱신(TUI는 매 변경마다 전체를 다시 그린다), GUI 표면, 음성 합성으로 가는 NUI.
- 문장의 언어. 지금 NUI의 틀 문장은 영어이고 값은 앱의 text 그대로다.
