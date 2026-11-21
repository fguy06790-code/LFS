#!/usr/bin/env python3
"""Plan a BLFS build from the BLFS book (single-page HTML).

Usage: blfs-book.py BOOK.html OUTDIR TARGET [TARGET...]
                    [--with ID,ID,...] [--lang en_US.UTF-8] [--keep-tests]

Reads every section of the book, follows the "Required" dependencies of the
targets (and of anything they pull in), plus any sections named with --with,
and writes them in build order:

  OUTDIR/plan.tsv        NNN <tab> id <tab> tarball-url-or-"-" <tab> md5-or-"-" <tab> title
  OUTDIR/NNN-ID.fetch    host-side download commands (bulk pages' wget blocks), if any
  OUTDIR/NNN-ID.sh       the section's build commands, run inside the chroot
  OUTDIR/NNN-ID.extra    extra files to download (patches), one URL per line
  OUTDIR/dropped.txt     command blocks left out, and why (placeholders, tests)

Only text inside <kbd class="command"> is taken, so explanations the book
prints after a command are not run. Blocks that are just "bash -e" or "exit"
(meant for someone typing at a terminal) are dropped, and "make install-NAME"
blocks are run from the blfs-bootscripts directory, as the book intends.
"""
import html.parser
import os
import re
import subprocess
import sys

# A block whose first command is one of these only runs tests: leave it out.
TEST_BLOCK_START = re.compile(
    r"^\s*(make\b[^\n]*\b(check|test|tests)\b|ninja\b[^\n]*\btest\b|meson test\b|"
    r"meson configure\b[^\n]*(test|assert)|ctest\b|pytest\b|python3 -m pytest\b|"
    r"[A-Z_]+=\S+ [A-Z_]+=\S+ ninja test|grep\b[^\n]*make_check\.log|su tester\b|"
    r"chown -R tester\b|make fate\b)")
# Inside a block that also builds, test commands are replaced with "true", so
# chains like "make && make test && make install" keep working.
TEST_INLINE = re.compile(
    r"\bmake(\s+-[\w=$()-]+)*\s+(-k\s+)?(check|test|tests)\b|\bninja\s+test\b|\bmeson\s+test\b")
DOWNLOAD_BLOCK = re.compile(r"\bwget\b|^\s*cat > \S+\.md5 <<", re.M)

# Placeholders with one sensible value for an automated build. A block with any
# other placeholder is left out and listed in dropped.txt.
SUBSTITUTE = {
    "<N>": "$(nproc)",
    "$(nproc)": "$(nproc)",
    # Locale pieces; filled from --lang (default en_US.UTF-8).
    "<ll>": "@LL@",
    "<CC>": "@CC@",
    "<charmap>": "@CHARMAP@",
    "<@modifiers>": "",
}
BOOTSCRIPT_BLOCK = re.compile(r"^\s*make install-[\w-]+\s*$")


class Section:
    def __init__(self, sid, title):
        self.id = sid
        self.title = title
        self.url = None
        self.md5 = None
        self.extra = []          # (label, url)
        self.deps = {"required": [], "recommended": [], "optional": [], "runtime": []}
        self.blocks = []         # (text, has_placeholder)


class BookParser(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.sections = {}
        self.order = []
        self.cur = None
        self.in_title = False
        self.title_id = None
        self.title_text = []
        self.dep_kind = None
        self.in_pre = False
        self.kbd_depth = 0
        self.pre_text = []
        self.pre_repl = False
        self.li_text = []
        self.in_li = False
        self.li_links = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        cls = (a.get("class") or "").split()
        if tag in ("h1", "h2", "h3") and ("title" in cls or "sect2" in cls):
            # h1/h2 "title" headings start a page. An h3 "sect2" heading with an id
            # starts a sub-package (like libinput inside Xorg input drivers) only if
            # a package-information block follows; otherwise it is an internal heading.
            self.in_title, self.title_id, self.title_text = True, None, []
            self.title_is_sub = "sect2" in cls
            self.pending_sub = None
        elif tag == "div" and "package" in cls and getattr(self, "pending_sub", None):
            sid, title = self.pending_sub
            self.pending_sub = None
            self.start_section(sid, title)
        elif tag == "a" and self.in_title and a.get("id") and not self.title_id:
            self.title_id = a["id"]
        elif tag == "p" and cls and cls[0] in self.cur_deps_kinds():
            self.dep_kind = cls[0]
        elif tag == "a" and self.dep_kind and self.cur and (a.get("href") or "").startswith("#"):
            dep = a["href"][1:]
            if dep not in self.cur.deps[self.dep_kind]:
                self.cur.deps[self.dep_kind].append(dep)
        elif tag == "pre" and {"userinput", "root"} & set(cls):
            self.in_pre, self.pre_text, self.pre_repl, self.kbd_depth = True, [], False, 0
            self.pre_repl_texts = []
        elif tag == "kbd" and self.in_pre and "command" in cls:
            self.kbd_depth += 1
        elif tag == "em" and self.in_pre and self.kbd_depth and "replaceable" in cls:
            self.pre_repl = True
            self.in_repl, self.repl_text = True, []
        elif tag == "li" and self.cur:
            self.in_li, self.li_text, self.li_links = True, [], []
        elif tag == "a" and self.in_li and (a.get("href") or "").startswith(("http", "ftp")):
            self.li_links.append(a["href"])

    def start_section(self, sid, title):
        self.cur = Section(sid, title)
        if sid not in self.sections:
            self.sections[sid] = self.cur
            self.order.append(sid)

    @staticmethod
    def cur_deps_kinds():
        return ("required", "recommended", "optional", "runtime")

    def handle_endtag(self, tag):
        if tag in ("h1", "h2", "h3") and self.in_title:
            self.in_title = False
            if self.title_id:
                title = re.sub(r"\s+", " ", "".join(self.title_text)).strip()
                if self.title_is_sub:
                    self.pending_sub = (self.title_id, title)
                else:
                    self.start_section(self.title_id, title)
        elif tag == "p":
            self.dep_kind = None
        elif tag == "em" and getattr(self, "in_repl", False):
            self.in_repl = False
            self.pre_repl_texts.append("".join(self.repl_text))
        elif tag == "kbd" and self.in_pre and self.kbd_depth:
            self.kbd_depth -= 1
        elif tag == "pre" and self.in_pre:
            self.in_pre = False
            text = "".join(self.pre_text).strip()
            if self.cur is not None and text:
                self.cur.blocks.append((text, list(self.pre_repl_texts)))
        elif tag == "li" and self.in_li:
            self.in_li = False
            self.package_info(re.sub(r"\s+", " ", "".join(self.li_text)).strip(), self.li_links)

    def handle_data(self, data):
        if self.in_title:
            self.title_text.append(data)
        if self.in_pre and self.kbd_depth:
            self.pre_text.append(data)
            if getattr(self, "in_repl", False):
                self.repl_text.append(data)
        if self.in_li:
            self.li_text.append(data)

    def package_info(self, text, links):
        s = self.cur
        if text.startswith("Download (HTTP):") and links and not s.url:
            s.url = links[0]
        elif text.startswith("Download (FTP):") and links and not s.url:
            s.url = links[0]
        elif text.startswith("Download MD5 sum:") and not s.md5:
            m = re.search(r"\b([0-9a-f]{32})\b", text)
            if m:
                s.md5 = m.group(1)
        # Optional patches too: the commands apply them (e.g. glib-skip_warnings).
        elif links and re.match(r"(Required|Recommended) (patch|download)|Optional patch", text):
            for u in links:
                if (text.split(":")[0], u) not in s.extra:
                    s.extra.append((text.split(":")[0], u))


# Pairs the book lists as depending on each other, where the second one is
# really built *after* the first: Shadow is reinstalled with PAM support once
# Linux-PAM exists, and Phonon's VLC backend needs Phonon itself.
POST_DEPS = {
    "linux-pam": ["shadow"],
    "phonon": ["phonon-backend-vlc"],
}


def plan(sections, targets, extra_ids):
    """Depth-first, dependencies first. Cycles are broken where they're found."""
    out, state = [], {}

    def visit(sid, path):
        if sid not in sections or state.get(sid) == "done":
            return
        if state.get(sid) == "active":
            print(f"note: dependency cycle broken at {' -> '.join(path + [sid])}", file=sys.stderr)
            return
        state[sid] = "active"
        post = POST_DEPS.get(sid, [])
        for dep in sections[sid].deps["required"]:
            if dep not in post:
                visit(dep, path + [sid])
        state[sid] = "done"
        out.append(sid)
        for dep in post:
            visit(dep, [])

    for t in list(extra_ids) + list(targets):
        if t not in sections:
            sys.exit(f"blfs-book: unknown section id: {t}")
        visit(t, [])
    return out


def main():
    argv = sys.argv[1:]
    keep_tests = "--keep-tests" in argv
    extra_ids = []
    if "--with" in argv:
        i = argv.index("--with")
        extra_ids = [x for x in argv[i + 1].split(",") if x]
        del argv[i:i + 2]
    lang = "en_US.UTF-8"
    if "--lang" in argv:
        i = argv.index("--lang")
        lang = argv[i + 1]
        del argv[i:i + 2]
    m = re.match(r"^([a-z]+)_([A-Z]+)\.(.+)$", lang)
    if not m:
        sys.exit(f"blfs-book: --lang must look like en_US.UTF-8, got {lang}")
    lang_ll, lang_cc, lang_charmap = m.groups()
    args = [a for a in argv if not a.startswith("--")]
    if len(args) < 3:
        sys.exit(__doc__)
    book, outdir, targets = args[0], args[1], args[2:]

    p = BookParser()
    with open(book, encoding="utf-8") as f:
        p.feed(f.read())

    order = plan(p.sections, targets, extra_ids)
    os.makedirs(outdir, exist_ok=True)
    dropped, manifest = [], []
    for n, sid in enumerate(order, 1):
        s = p.sections[sid]
        num = f"{n:03d}"
        fetch, build = [], []
        for text, repl in s.blocks:
            first = text.strip()
            # "bash -e" and "exit" are for someone typing at a terminal; a bare
            # "exit" at the end of a bulk loop would stop the script early.
            text = "\n".join(l for l in text.splitlines() if l.strip() not in ("bash -e", "exit"))
            first = text.strip()
            if not first:
                continue
            if not keep_tests:
                code = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))
                if TEST_BLOCK_START.match(code):
                    dropped.append(f"{sid}\ttest\t{first.splitlines()[0][:70]}")
                    continue
                text = "\n".join(l if l.lstrip().startswith("#") else TEST_INLINE.sub("true", l)
                                 for l in text.splitlines())
                first = text.strip()
            if repl:
                if all(r in SUBSTITUTE for r in repl):
                    for r in repl:
                        text = text.replace(r, SUBSTITUTE[r])
                    text = (text.replace("@LL@", lang_ll).replace("@CC@", lang_cc)
                                .replace("@CHARMAP@", lang_charmap))
                    first = text.strip()
                else:
                    unknown = ", ".join(sorted(set(r for r in repl if r not in SUBSTITUTE)))
                    dropped.append(f"{sid}\tplaceholder {unknown}\t{first.splitlines()[0][:70]}")
                    continue
            # Example commands with unmarked placeholders (like "ldd <library>")
            # are not valid shell; leave them out.
            if subprocess.run(["bash", "-n"], input=text, text=True,
                              capture_output=True).returncode != 0:
                dropped.append(f"{sid}\tnot valid shell\t{first.splitlines()[0][:70]}")
                continue
            if DOWNLOAD_BLOCK.search(text):
                fetch.append(text)
            if BOOTSCRIPT_BLOCK.match(text):
                text = f'( cd "$BLFS_BOOTSCRIPTS" && {first} )'
            build.append(text)
        base = os.path.join(outdir, f"{num}-{sid}")
        with open(base + ".sh", "w", newline="\n") as f:
            f.write(f"# {s.title}\n# section: {sid}\n\n" + "\n\n".join(build) + "\n")
        if fetch:
            with open(base + ".fetch", "w", newline="\n") as f:
                f.write(f"# {s.title}: downloads\n\n" + "\n\n".join(fetch) + "\n")
        if s.extra:
            with open(base + ".extra", "w", newline="\n") as f:
                f.write("\n".join(u for _, u in s.extra) + "\n")
        manifest.append(f"{num}\t{sid}\t{s.url or '-'}\t{s.md5 or '-'}\t{s.title}")

    with open(os.path.join(outdir, "plan.tsv"), "w", newline="\n") as f:
        f.write("\n".join(manifest) + "\n")
    with open(os.path.join(outdir, "dropped.txt"), "w", newline="\n") as f:
        f.write("\n".join(dropped) + ("\n" if dropped else ""))
    print(f"planned {len(order)} BLFS sections into {outdir} "
          f"({len(dropped)} command blocks left out; see dropped.txt)")


if __name__ == "__main__":
    main()
