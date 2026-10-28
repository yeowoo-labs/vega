# Vega

Surfaces for [aup](https://github.com/yeowoo-labs/aup) apps: one specification, many surfaces.

Vega is an aup library (`use vega`) plus a set of hosts. An app declares lists, tables, focus and
tone once; Vega shows that declaration as a terminal UI, non-interactive CLI output, spoken
sentences, a web page, pixels or a framebuffer — without copying the data into a per-surface model.

**Status: v0, pre-1.0.** The first components (lists, tables, focus, tone) and four surfaces work:
TUI, CLI, speech and web, plus pixel and display output. No release, no compatibility promise.
There has been no external security audit.

## Build and run

`make` links `libpure.a` from a Pure checkout beside this one.

```sh
git clone https://github.com/yeowoo-labs/pure.git ../pure && make -C ../pure
git clone https://github.com/yeowoo-labs/aup.git ../aup && make -C ../aup
make                                                      # build/vega, build/vega-symbols

AUP_PATH=lib ../aup/build/aup build examples/todo.aup -o todo.pmod
build/vega tui todo.pmod --store ~/.todo                  # keyboard; Tab for key mode, Ctrl-C to quit
printf 'text 6d696c6b\nkey Space\nshow\n' | build/vega cli todo.pmod
printf 'text 6d696c6b\nkey Space\nshow\n' | build/vega say todo.pmod
build/vega web todo.pmod --watch                          # browser; follows rebuilt modules
make check                                                # reference models, frames, pty, robustness
```

## The same app for a person and a program

`examples/board.aup` is a kanban board. A person uses a Vega surface; a program uses the same
module and store through Pure's `pure-serve`, with no route code — views are `GET`, inputs are
`POST`.

```sh
AUP_PATH=lib ../aup/build/aup build examples/board.aup -o board.pmod
build/vega tui board.pmod --store ~/.board
../pure/build/pure-serve --module board.pmod --store ~/.board --threads 2 &
curl -s localhost:8767/views/counts
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"title": "ship it", "lane": 2}' localhost:8767/inputs/post
```

One store is opened by one host at a time — the owner of state is single. To write from a person
and a program at once, run the surface inside the node: `pure-serve` maps `build/vega-surface.so`
and runs it on its own thread, drawing from the last commit in place. Keystrokes and text travel on
the same path as HTTP requests, with no HTTP or JSON in between.

```sh
../pure/build/pure-serve --module board.pmod --store ~/.board \
    --surface build/vega-surface.so -- display --symbols ui.vsym --live
```

`examples/messenger.aup` is a two-node messenger. A line you type is stored, then sent to the other
node, sealed to its key and authenticated by yours, so a relay between them can neither read nor
change it. The screen is Vega inside each node.

## Symbols

Text and icons are drawn from a single binary symbol format. Fonts and SVG are resolved once, at
build time.

```sh
build/vega-symbols build --font DejaVuSansMono.ttf --codes 0x20-0x7e \
    --font NotoSansCJK-Regular.ttc --face 6 --codes 0xac00-0xd7a3 \
    --svg 0xE000=icon.svg -o ui.vsym
build/vega-symbols compare DejaVuSans.ttf ui.vsym --sizes 12,16,32
build/vega-symbols render ui.vsym 0xE000 48 icon.pgm
```

## Layout

| Path | What lives there |
|---|---|
| `lib/` | The aup library (`use vega`): lists, tables, focus, tone |
| `surfaces/` | TUI, CLI, speech, web, pixels, display (KMS), and the in-node surface library |
| `symbols/` | One glyph format, drawn on CPU or GPU |
| `docs/design/` | Surface and symbol design |
| `tests/` | Reference models, frame checks, pty and robustness tests |

## Where these fit

| Layer | Role | Analogy |
|---|---|---|
| [Pure](https://github.com/yeowoo-labs/pure) | Runtime: execution, memory and ownership, persisted state, transport | JVM |
| [aup](https://github.com/yeowoo-labs/aup) | The app declaration: written, checked, built | Java and javac |
| **Vega** | The surface library and hosts | JavaFX |

## Principles

- **No lowest common denominator.** Each surface states what it supports; a surface never silently
  gives a feature a different meaning.
- **No per-surface copies of data.** Values are borrowed from the runtime's persisted state, not
  rebuilt into a surface model.
- **No Galápagos.** ANSI terminals, shell pipes and platform accessibility APIs stay the points of
  contact.

## License

Apache-2.0. See [LICENSE](LICENSE).
