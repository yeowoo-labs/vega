#!/usr/bin/env python3
"""Bring in Vega's text upstreams, and hold Vega to them:

    python3 text/upstream.py [--harfbuzz VERSION] [--unicode VERSION] [--corpus] [--offline]

Vega's shaping is HarfBuzz's, case for case, and its Unicode tables are
generated from the Unicode Character Database. When either moves on, this
is how Vega follows (docs/design/004-text.md):

  1. fetch HarfBuzz VERSION's release (default: the one the build links,
     pkg-config) and the UCD of the Unicode version its tables came from
     (its src/hb-ucd-table.hh says; --unicode to choose), once, into
     build/upstream;
  2. build that HarfBuzz as one static library, from its amalgamated
     harfbuzz.cc: no system package, any version;
  3. regenerate text/unicode-data.c (text/unicode.py) and
     text/syllabic-data.c (text/syllabic.py: the syllable machines and the
     Indic, USE and Arabic tables), and hold the machines to HarfBuzz's own
     compiled ones (syllabic.py --check);
  4. build Vega's text tools against that HarfBuzz (build/upstream/build)
     and run make check-text: Unicode's conformance tests on that UCD,
     random shaping against that HarfBuzz, the layout's promises, the kern
     class tests, and HarfBuzz's own test suite, case by case;
  5. with --corpus, every installed font against it on random runs
     (text/fonts.py).

What a new version breaks shows as failing checks, each with the font and
the input (the suite's case, the random run) to port its fix from. The
generated files are left changed for review (git diff). One JSON line at
the end: the versions, what changed, how each check went; exit 1 when one
failed. --offline uses only what build/upstream already holds.
"""
import argparse
import json
import re
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UPSTREAM = ROOT / 'build' / 'upstream'


def fetch(url, into, offline):
    if into.exists():
        return
    if offline:
        sys.exit(f'upstream: {into} is not here, and --offline')
    print(f'fetching {url}', file=sys.stderr)
    part = into.with_name(into.name + '.part')
    with urllib.request.urlopen(url, timeout=300) as response, open(part, 'wb') as out:
        while chunk := response.read(1 << 20):
            out.write(chunk)
    part.rename(into)


def harfbuzz(version, offline):
    """The release's source, and a static library built from it."""
    source = UPSTREAM / f'harfbuzz-{version}'
    if not (source / 'src' / 'harfbuzz.cc').exists():
        archive = UPSTREAM / f'harfbuzz-{version}.tar.xz'
        fetch(f'https://github.com/harfbuzz/harfbuzz/releases/download/{version}/harfbuzz-{version}.tar.xz', archive,
              offline)
        with tarfile.open(archive) as tar:
            tar.extractall(UPSTREAM, filter='data')
    library = source / 'libharfbuzz.a'
    if not library.exists():
        print(f'building HarfBuzz {version}', file=sys.stderr)
        subprocess.run(['c++', '-O2', '-fPIC', '-std=c++17', '-DNDEBUG', '-c', str(source / 'src' / 'harfbuzz.cc'),
                        '-o', str(source / 'harfbuzz.o')], check=True)
        subprocess.run(['ar', 'rcs', str(library), str(source / 'harfbuzz.o')], check=True)
    return source, library


def ucd(version, offline):
    directory = UPSTREAM / f'ucd-{version}'
    if not (directory / 'ReadMe.txt').exists():
        archive = UPSTREAM / f'UCD-{version}.zip'
        fetch(f'https://www.unicode.org/Public/{version}/ucd/UCD.zip', archive, offline)
        with zipfile.ZipFile(archive) as z:
            z.extractall(directory)
    return directory


def run(name, command, results, **kwargs):
    """A step: its last JSON line (if any) and whether it passed."""
    print(f'== {name}', file=sys.stderr)
    done = subprocess.run(command, capture_output=True, text=True, cwd=ROOT, **kwargs)
    sys.stderr.write(done.stderr[-4000:])
    reports = [json.loads(line) for line in done.stdout.splitlines() if line.startswith('{')]
    results[name] = {'passed': done.returncode == 0, **({'report': reports} if reports else {})}
    if done.returncode:
        sys.stderr.write(done.stdout[-4000:])
    return done


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--harfbuzz')
    parser.add_argument('--unicode')
    parser.add_argument('--corpus', action='store_true')
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    UPSTREAM.mkdir(parents=True, exist_ok=True)
    version = args.harfbuzz or subprocess.run(['pkg-config', '--modversion', 'harfbuzz'], capture_output=True,
                                              text=True, check=True).stdout.strip()
    source, library = harfbuzz(version, args.offline)
    named = re.search(r'Unicode (\d+\.\d+\.\d+)', (source / 'src' / 'hb-ucd-table.hh').read_text())
    unicode = args.unicode or named.group(1)
    database = ucd(unicode, args.offline)

    results = {}
    generated = ['text/unicode-data.c', 'text/syllabic-data.c']
    with open(ROOT / 'text' / 'unicode-data.c', 'w') as out:
        results['unicode.py'] = {'passed': subprocess.run([sys.executable, 'text/unicode.py', str(database)],
                                                          stdout=out, cwd=ROOT).returncode == 0}
    with open(ROOT / 'text' / 'syllabic-data.c', 'w') as out:
        results['syllabic.py'] = {'passed': subprocess.run([sys.executable, 'text/syllabic.py', str(source / 'src')],
                                                           stdout=out, cwd=ROOT).returncode == 0}
    run('syllabic.py --check', [sys.executable, 'text/syllabic.py', str(source / 'src'), '--check'], results)

    build = UPSTREAM / 'build'
    flags = [f'BUILD={build}', f'HARFBUZZ_CFLAGS=-I{source / "src"}',
             f'HARFBUZZ_LIBS={library} -lstdc++ -lm', f'UCD={database}', f'HARFBUZZ_SRC={source}']
    run('check-text', ['make', '-s', 'check-text'] + flags, results)
    if args.corpus:
        run('fonts', [sys.executable, 'text/fonts.py', str(build / 'vega-symbols')], results)

    changed = subprocess.run(['git', 'diff', '--stat', '--'] + generated, capture_output=True, text=True,
                             cwd=ROOT).stdout.strip().splitlines()
    print(json.dumps({'harfbuzz': version, 'unicode': unicode, 'changed': changed[:-1] if changed else [],
                      'checks': results}))
    return 0 if all(r['passed'] for r in results.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
