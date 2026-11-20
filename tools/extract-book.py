#!/usr/bin/env python3
"""Turn the LFS book (single-page HTML) into numbered build scripts.

Usage: extract-book.py BOOK.html WGET-LIST OUTDIR [--keep-tests]

For every section in chapters 5-10 that has commands, writes
OUTDIR/NNN-<section-id>.sh with the commands from its <pre class="userinput">
and <pre class="root"> blocks, in book order, and OUTDIR/manifest.tsv with
one line per section:

    NNN <tab> chapter <tab> section-id <tab> tarball-or-"-" <tab> flags <tab> title

flags: "R" when a block contains a placeholder the reader must fill in
(<em class="replaceable">), "-" otherwise. The build script refuses to run a
section flagged R unless it has an override.

Test-suite blocks are dropped unless --keep-tests is given: they are optional
in the book and roughly double the build time.
"""
import html.parser
import os
import re
import sys

TEST_PATTERNS = [
    re.compile(r"\bmake\b[^\n]*\b(check|test|tests)\b"),
    re.compile(r"\bninja\b[^\n]*\btest\b"),
    re.compile(r"\bsu tester\b"),
    re.compile(r"\btest_summary\b"),
    re.compile(r"\bctest\b"),
    re.compile(r"chown\s+(-R\s+)?tester\b"),
    re.compile(r"\bpytest\b"),
    # Inspecting test results ("grep FAIL $(find -name '*.log')") needs the tests to have run.
    re.compile(r"^grep .*\$\(find -name ", re.M),
]


class BookParser(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.sections = []          # dicts: id, title, blocks
        self.cur = None
        self.in_title = False
        self.title_depth = 0
        self.pending_id = None
        self.in_pre = False
        self.pre_text = []
        self.pre_replaceable = False

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "h2" and "title" in (a.get("class") or "").split():
            self.in_title = True
            self.title_text = []
            self.pending_id = None
        elif tag == "a" and self.in_title and a.get("id", "").startswith("ch-"):
            self.pending_id = a["id"]
        elif tag == "pre" and {"userinput", "root"} & set((a.get("class") or "").split()):
            self.in_pre = True
            self.pre_text = []
            self.pre_replaceable = False
        elif tag == "em" and self.in_pre and "replaceable" in (a.get("class") or "").split():
            self.pre_replaceable = True

    def handle_endtag(self, tag):
        if tag == "h2" and self.in_title:
            self.in_title = False
            title = re.sub(r"\s+", " ", "".join(self.title_text)).strip()
            if self.pending_id:
                self.cur = {"id": self.pending_id, "title": title, "blocks": []}
                self.sections.append(self.cur)
        elif tag == "pre" and self.in_pre:
            self.in_pre = False
            if self.cur is not None:
                self.cur["blocks"].append(("".join(self.pre_text), self.pre_replaceable))

    def handle_data(self, data):
        if self.in_title:
            self.title_text.append(data)
        elif self.in_pre:
            self.pre_text.append(data)


def chapter_of(title):
    m = re.match(r"(\d+)\.\d+\.", title)
    return int(m.group(1)) if m else 0


def package_name(title):
    """'5.2. Binutils-2.45 - Pass 1' -> 'Binutils-2.45'."""
    t = re.sub(r"^\d+\.\d+\.\s*", "", title)
    t = re.split(r"\s+-\s+Pass\s+\d", t)[0]
    if " from " in t:                       # 'Libstdc++ from GCC-15.2.0'
        t = t.split(" from ", 1)[1]
    t = t.split(" ")[0]                     # 'Linux-6.16.1 API Headers'
    return t.replace("::", "-")             # 'XML::Parser-2.47'


def find_tarball(name, tarballs):
    """Match a package name to a tarball in the source list."""
    m = re.match(r"^(.*?)-(\d.*)$", name)  # version starts at the first "-<digit>"
    if not m:
        return None
    lower = {t.lower(): t for t in tarballs}
    base, ver = m.group(1), m.group(2)
    candidates = [
        name,
        base + ver,                         # tcl8.6.16-src, expect5.45.4
        base.replace("-", "_") + "-" + ver, # flit_core-3.12.0
    ]
    for c in candidates:
        c = c.lower()
        for low, real in lower.items():
            if (low.startswith(c + ".tar") or low.startswith(c + "-src.tar")
                    or low == c + ".tgz"):
                return real
    return None


def is_test_block(text):
    return any(p.search(text) for p in TEST_PATTERNS)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    keep_tests = "--keep-tests" in sys.argv
    if len(args) != 3:
        sys.exit(__doc__)
    book, wget_list, outdir = args

    with open(wget_list) as f:
        tarballs = [os.path.basename(l.strip()) for l in f if l.strip()]
    tarballs = [t for t in tarballs if ".tar" in t or t.endswith(".tgz")]

    p = BookParser()
    with open(book, encoding="utf-8") as f:
        p.feed(f.read())

    os.makedirs(outdir, exist_ok=True)
    manifest = []
    n = 0
    for s in p.sections:
        ch = chapter_of(s["title"])
        if ch < 5 or ch > 10:
            continue
        blocks = [(t, r) for t, r in s["blocks"] if keep_tests or not is_test_block(t)]
        if not blocks:
            continue
        n += 1
        tarball = find_tarball(package_name(s["title"]), tarballs) if ch in (5, 6, 7, 8, 9, 10) else None
        flag = "R" if any(r for _, r in blocks) else "-"
        num = f"{n:03d}"
        path = os.path.join(outdir, f"{num}-{s['id']}.sh")
        with open(path, "w", newline="\n") as out:
            out.write(f"# {s['title']}\n")
            out.write(f"# section: {s['id']}   package: {tarball or '-'}\n\n")
            for text, _ in blocks:
                out.write(text.rstrip() + "\n\n")
        manifest.append(f"{num}\t{ch}\t{s['id']}\t{tarball or '-'}\t{flag}\t{s['title']}")

    with open(os.path.join(outdir, "manifest.tsv"), "w", newline="\n") as m:
        m.write("\n".join(manifest) + "\n")
    print(f"extracted {n} sections into {outdir}")


if __name__ == "__main__":
    main()
