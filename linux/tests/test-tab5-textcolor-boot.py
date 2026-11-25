#!/usr/bin/env python3
"""The Tab5 console in its text_color= from the first kernel line.

The boot shim reads text_color= from config.txt in the boot FAT slot and
writes the preset into the vt.default_red/_grn/_blu= placeholder of the
DTB bootargs (linux/boot-shim/main/tab5_textcolor.c).  This test checks:

- the shim's preset table equals textcolor.sh, the one S07textcolor and
  m5-config use, and textcolor_vtdefaults' stock lists are the kernel's;
- the Tab5 DTS carries the placeholder with the kernel's own stock
  palette in the fixed "0xNN" x 16 form, so an unpatched DTB changes
  nothing;
- tab5-textcolor-harness.c, built with the shim's file, against FAT12
  images made here: the line rules of S07textcolor (blanks, CRLF, the
  first key wins), long-name, deleted, label and directory entries, a
  fragmented file read across clusters, broken volumes and chains, and
  the bootargs it writes.  With mkfs.fat and mcopy at hand it also reads
  an image made the way post-build.sh makes the boot FAT.

The harness needs a C compiler: a native cc/gcc, or else (outside CI) the
first running docker container.  Without one that part is skipped, except
in CI where that is a failure.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHIM = os.path.join(ROOT, "linux", "boot-shim", "main")
C_SRC = os.path.join(SHIM, "tab5_textcolor.c")
C_HDR = os.path.join(SHIM, "tab5_textcolor.h")
HARNESS = os.path.join(ROOT, "linux", "tests", "tab5-textcolor-harness.c")
TCLIB = os.path.join(ROOT, "linux", "m3-lab", "rootfs-overlay-tab5", "usr",
                     "lib", "board", "textcolor.sh")
DTS = os.path.join(ROOT, "linux", "dts", "esp32p4-tab5.dts")

# drivers/tty/vt/vt.c default_red/_grn/_blu: the stock VT palette.
STOCK = {
    "red": [0x00, 0xaa, 0x00, 0xaa, 0x00, 0xaa, 0x00, 0xaa,
            0x55, 0xff, 0x55, 0xff, 0x55, 0xff, 0x55, 0xff],
    "grn": [0x00, 0x00, 0xaa, 0x55, 0x00, 0x00, 0xaa, 0xaa,
            0x55, 0x55, 0xff, 0xff, 0x55, 0x55, 0xff, 0xff],
    "blu": [0x00, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0xaa, 0xaa,
            0x55, 0x55, 0x55, 0x55, 0xff, 0xff, 0xff, 0xff],
}
CHANNELS = ("red", "grn", "blu")
# riscv COMMAND_LINE_SIZE: the kernel cuts the bootargs there.
COMMAND_LINE_SIZE = 2048

checks = 0
fails = 0


def ok(msg):
    global checks
    checks += 1
    print(f"ok: {msg}")


def bad(msg):
    global checks, fails
    checks += 1
    fails += 1
    print(f"FAIL: {msg}")


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


# --- the tables -----------------------------------------------------------

def shell_presets():
    """textcolor_seq's _tc_mono rows: name -> (normal, bright, dim, bg)."""
    out = {}
    for m in re.finditer(r"^\s*(\w+)\)\s*_tc_mono\s+([0-9a-f]{6})\s+"
                         r"([0-9a-f]{6})\s+([0-9a-f]{6})(?:\s+([0-9a-f]{6}))?"
                         r"\s*;;", read(TCLIB), re.M):
        out[m.group(1)] = tuple(int(v or "000000", 16) for v in m.group(2, 3, 4, 5))
    return out


def c_presets():
    """tab5_textcolor.c's tc_presets[]: name -> (normal, bright, dim, bg)."""
    out = {}
    for m in re.finditer(r'\{\s*"(\w+)",\s*0x([0-9a-f]{6}),\s*0x([0-9a-f]{6}),'
                         r'\s*0x([0-9a-f]{6}),\s*0x([0-9a-f]{6})\s*\}',
                         read(C_SRC)):
        out[m.group(1)] = tuple(int(v, 16) for v in m.group(2, 3, 4, 5))
    return out


def palette(preset):
    """16 slots (r, g, b): slot 0 bg, 1-7 normal, 8 dim, 9-15 bright."""
    normal, bright, dim, bg = preset
    slots = [bg] + [normal] * 7 + [dim] + [bright] * 7
    return [((v >> 16) & 255, (v >> 8) & 255, v & 255) for v in slots]


def stock_param(ch):
    return f"vt.default_{ch}=" + ",".join(f"0x{v:02x}" for v in STOCK[ch])


def patched_args(args, slots):
    for i, ch in enumerate(CHANNELS):
        new = f"vt.default_{ch}=" + ",".join(f"0x{s[i]:02x}" for s in slots)
        args = args.replace(stock_param(ch), new)
    return args


def dts_bootargs():
    m = re.findall(r'^\s*bootargs = "([^"]*)";', read(DTS), re.M)
    return m[0] if len(m) == 1 else None


def static_checks(shp, cp):
    if shp and shp == cp and list(shp) == list(cp):
        ok(f"shim preset table equals textcolor.sh ({len(cp)} presets)")
    else:
        bad(f"preset tables differ: textcolor.sh {shp} vs shim {cp}")
    m = re.search(r'^TEXTCOLOR_NAMES="([^"]*)"', read(TCLIB), re.M)
    names = m.group(1).split() if m else []
    if names and names[0] == "white" and names[1:] == list(cp):
        ok("TEXTCOLOR_NAMES is white + the shim's presets")
    else:
        bad(f"TEXTCOLOR_NAMES {names} vs shim {list(cp)}")
    lib = read(TCLIB)
    for ch, var in zip(CHANNELS, ("_tcv_r", "_tcv_g", "_tcv_b")):
        want = ",".join(str(v) for v in STOCK[ch])
        if re.search(rf"^\s*{var}={re.escape(want)}\s*$", lib, re.M):
            ok(f"textcolor_vtdefaults stock {ch} is the kernel's")
        else:
            bad(f"textcolor_vtdefaults stock {ch} differs from {want}")

    args = dts_bootargs()
    if args is None:
        bad("esp32p4-tab5.dts: not exactly one bootargs")
        return None
    for ch in CHANNELS:
        p = stock_param(ch)
        if args.split().count(p) == 1 and args.count(f"vt.default_{ch}=") == 1:
            ok(f"DTS bootargs carry the stock {ch} placeholder once")
        else:
            bad(f"DTS bootargs lack the exact stock {p}")
    if len(args) < COMMAND_LINE_SIZE:
        ok(f"bootargs ({len(args)} chars) fit COMMAND_LINE_SIZE")
    else:
        bad(f"bootargs {len(args)} chars >= {COMMAND_LINE_SIZE}")
    return args


# --- FAT12 images ---------------------------------------------------------

def lfn_checksum(short):
    s = 0
    for c in short:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(name, short):
    chars = [ord(c) for c in name] + [0]
    while len(chars) % 13:
        chars.append(0xFFFF)
    pieces = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    out = []
    for seq in range(len(pieces), 0, -1):
        p = pieces[seq - 1]
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == len(pieces) else 0)
        e[11] = 0x0F
        e[13] = lfn_checksum(short)
        for j, off in enumerate([1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]):
            e[off:off + 2] = p[j].to_bytes(2, "little")
        out.append(bytes(e))
    return out


class Fat12:
    """A FAT12 volume like mkfs.fat -F 12 -r 64 makes: 1 reserved sector,
    two FATs, 64 root entries."""

    def __init__(self, spc=1, total=512, bps=512, root_ents=64):
        self.bps, self.spc, self.total, self.root_ents = bps, spc, total, root_ents
        self.rsv, self.nfats = 1, 2
        self.root_secs = (root_ents * 32 + bps - 1) // bps
        self.fat_secs = 1
        while True:
            data = total - self.rsv - self.nfats * self.fat_secs - self.root_secs
            self.clusters = data // spc
            if ((self.clusters + 2) * 3 + 1) // 2 <= self.fat_secs * bps:
                break
            self.fat_secs += 1
        self.fat = [0] * (self.clusters + 2)
        self.fat[0], self.fat[1] = 0xFF8, 0xFFF
        self.entries = []
        self.data = bytearray(self.clusters * spc * bps)
        self.next = 2

    def entry(self, short, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = short
        e[11] = attr
        e[26:28] = cluster.to_bytes(2, "little")
        e[28:32] = size.to_bytes(4, "little")
        self.entries.append(bytes(e))

    def add(self, short, content, attr=0x20, lfn=None, chain=None):
        """A file; chain names its clusters (default: the next free)."""
        csize = self.spc * self.bps
        n = (len(content) + csize - 1) // csize
        if chain is None:
            chain = list(range(self.next, self.next + n))
            self.next += n
        assert len(chain) == n
        for i, c in enumerate(chain):
            part = content[i * csize:(i + 1) * csize]
            off = (c - 2) * csize
            self.data[off:off + len(part)] = part
            self.fat[c] = chain[i + 1] if i + 1 < n else 0xFFF
        if lfn:
            self.entries.extend(lfn_entries(lfn, short))
        self.entry(short, attr, chain[0] if chain else 0, len(content))
        return chain

    def image(self):
        bs = bytearray(self.bps)
        bs[0:3] = b"\xeb\x3c\x90"
        bs[3:11] = b"mkfs.fat"
        bs[11:13] = self.bps.to_bytes(2, "little")
        bs[13] = self.spc
        bs[14:16] = self.rsv.to_bytes(2, "little")
        bs[16] = self.nfats
        bs[17:19] = self.root_ents.to_bytes(2, "little")
        if self.total < 0x10000:
            bs[19:21] = self.total.to_bytes(2, "little")
        else:
            bs[32:36] = self.total.to_bytes(4, "little")
        bs[21] = 0xF8
        bs[22:24] = self.fat_secs.to_bytes(2, "little")
        bs[24:26] = (32).to_bytes(2, "little")
        bs[26:28] = (2).to_bytes(2, "little")
        bs[36], bs[38] = 0x80, 0x29
        bs[43:54] = b"TAB5       "
        bs[54:62] = b"FAT12   "
        bs[510:512] = b"\x55\xaa"
        fat = bytearray(self.fat_secs * self.bps)
        for n, v in enumerate(self.fat):
            off = n + n // 2
            if n & 1:
                fat[off] = (fat[off] & 0x0F) | ((v & 0x0F) << 4)
                fat[off + 1] = v >> 4
            else:
                fat[off] = v & 0xFF
                fat[off + 1] = (fat[off + 1] & 0xF0) | (v >> 8)
        root = bytearray(self.root_secs * self.bps)
        for i, e in enumerate(self.entries):
            root[i * 32:(i + 1) * 32] = e
        img = bs + bytes(self.bps * (self.rsv - 1)) + fat * self.nfats + root + self.data
        return bytes(img).ljust(self.total * self.bps, b"\0")[:self.total * self.bps]


CFG = b"CONFIG  TXT"
FILLER = (b"# Tab5 Boot & Hardware Configuration\n"
          b"hostname=tab5\n#wifi_mode=ap\n#text_color=green\n")


def one_file(content, **kw):
    v = Fat12(**{k: kw.pop(k) for k in ("spc",) if k in kw})
    v.entry(b"TAB5       ", 0x08, 0, 0)
    v.add(CFG, content, lfn="config.txt", **kw)
    return v.image()


def cases():
    """(label, image bytes, rc, name, preset name or None to patch with)."""
    out = []

    def case(label, img, rc, name, preset=None):
        out.append((label, img, rc, name, preset))

    case("amber after other lines", one_file(FILLER + b"text_color=amber\n"),
         1, "amber", "amber")
    case("blanks and CRLF around the value",
         one_file(b"  text_color=  amber  \r\nhostname=x\r\n"), 1, "amber", "amber")
    case("tab before the key", one_file(b"\ttext_color=lcd\n"), 1, "lcd", "lcd")
    case("the first text_color= wins",
         one_file(b"text_color=lcd\ntext_color=amber\n"), 1, "lcd", "lcd")
    case("an empty first value wins too",
         one_file(b"text_color=\ntext_color=amber\n"), 1, "", None)
    case("the last line without a newline", one_file(FILLER + b"text_color=cyan"),
         1, "cyan", "cyan")
    case("a commented key only", one_file(FILLER), 0, "", None)
    case("'text_color =' is not the key", one_file(b"text_color =amber\n"),
         0, "", None)
    case("white is no preset", one_file(b"text_color=white\n"), 1, "white", None)
    case("unknown name", one_file(b"text_color=purple\n"), 1, "purple", None)
    case("a value longer than the buffer comes back empty",
         one_file(b"text_color=amber" + b"x" * 30 + b"\n"), 1, "", None)
    case("trailing blanks past the buffer are trimmed",
         one_file(b"text_color=amber" + b" " * 40 + b"\n"), 1, "amber", "amber")
    case("a CR inside the line stays (S07 drops one)",
         one_file(b"text_color=amber\r\r\n"), 1, "amber\r", None)
    case("empty config.txt", one_file(b""), 0, "", None)

    # A fragmented file, the key across a cluster boundary: 512-byte
    # clusters, chain 9 -> 4 -> 12.
    body = bytearray(b"# " + b"x" * 500 + b"\n")       # 503 bytes
    body += b"#" * 3 + b"\n"                             # to 507
    body += b"text_color=green\n"                        # crosses 512
    body += b"# " + b"y" * 600 + b"\n"
    v = Fat12(spc=1)
    v.entry(b"TAB5       ", 0x08, 0, 0)
    v.add(CFG, bytes(body), lfn="config.txt", chain=[9, 4, 12])
    case("fragmented file, key across clusters", v.image(), 1, "green", "green")

    # 4-sector clusters (what mkfs.fat picks for 256 KB), the key in the
    # third cluster.
    v = Fat12(spc=4)
    v.add(CFG, b"#" * 4500 + b"\ntext_color=blue\n")
    case("2 KB clusters, the key in cluster 3", v.image(), 1, "blue", "blue")

    # Entries the reader must step over: a deleted CONFIG.TXT naming
    # red, a directory with the 8.3 name, other files.
    v = Fat12()
    v.entry(b"TAB5       ", 0x08, 0, 0)
    v.add(b"WPA_SU~1CON", b"network={}\n", lfn="wpa_supplicant.conf")
    gone = v.add(CFG, b"text_color=red\n")
    v.entries[-1] = b"\xe5" + v.entries[-1][1:]
    v.fat[gone[0]] = 0
    v.add(CFG, b"", attr=0x10)
    v.add(CFG, b"text_color=pink\n", lfn="config.txt")
    case("deleted, directory and other entries skipped", v.image(), 1, "pink", "pink")

    v = Fat12()
    v.add(b"CMDLINE TXT", b"text_color=red\n")
    case("no config.txt", v.image(), 0, "", None)

    img = bytearray(one_file(b"text_color=amber\n"))
    img[510] = 0
    case("no boot signature", bytes(img), -1, "", None)

    img = bytearray(one_file(b"text_color=amber\n"))
    img[19:21] = (40000).to_bytes(2, "little")  # > 4084 clusters: FAT16
    case("a FAT16-sized volume", bytes(img), -1, "", None)

    # A loop before the key with a size the loop never reaches: the
    # hop count has to end it.
    v = Fat12()
    chain = v.add(CFG, b"#" * 1500 + b"\ntext_color=amber\n")
    v.fat[chain[1]] = chain[0]
    v.entries[-1] = v.entries[-1][:28] + (10 << 20).to_bytes(4, "little")
    case("a looping chain", v.image(), -1, "", None)

    v = Fat12()
    chain = v.add(CFG, b"#" * 1500 + b"\ntext_color=amber\n")
    v.fat[chain[1]] = 0xFFF         # ends before the size
    case("a chain shorter than the file", v.image(), -1, "", None)

    v = Fat12()
    v.add(CFG, b"text_color=amber\n")
    v.entries[-1] = (v.entries[-1][:26] + (v.clusters + 5).to_bytes(2, "little")
                     + v.entries[-1][28:])
    case("a start cluster outside the volume", v.image(), -1, "", None)

    case("a truncated image", one_file(b"text_color=amber\n")[:1024], -1, "", None)
    return out


# --- the harness ----------------------------------------------------------

class Native:
    def __init__(self, cc, tmp):
        self.cc, self.tmp = cc, tmp
        self.exe = os.path.join(tmp, "harness")

    def build(self):
        r = subprocess.run([self.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra",
                            "-Werror", "-I", SHIM, "-o", self.exe, HARNESS, C_SRC],
                           capture_output=True, text=True)
        return r.returncode == 0, r.stdout + r.stderr

    def put(self, name, data):
        with open(os.path.join(self.tmp, name), "wb") as f:
            f.write(data)

    def run(self, name, args):
        r = subprocess.run([self.exe, os.path.join(self.tmp, name), args],
                           capture_output=True, text=True)
        return r.returncode, r.stdout

    def sh(self, cmd):
        r = subprocess.run(["sh", "-c", cmd], cwd=self.tmp, capture_output=True,
                           text=True)
        return r.returncode, r.stdout + r.stderr


class Docker:
    def __init__(self, ct, tmp):
        self.ct, self.tmp = ct, tmp
        self.rdir = f"/tmp/tab5-textcolor-{os.getpid()}"

    def _x(self, *cmd):
        r = subprocess.run(["docker", "exec", self.ct, *cmd],
                           capture_output=True, text=True)
        return r.returncode, (r.stdout + r.stderr).replace("\r", "")

    def build(self):
        self._x("mkdir", "-p", self.rdir)
        for src in (HARNESS, C_SRC, C_HDR):
            subprocess.run(["docker", "cp", src,
                            f"{self.ct}:{self.rdir}/{os.path.basename(src)}"],
                           check=True, capture_output=True)
        rc, out = self._x("cc", "-std=gnu11", "-O2", "-Wall", "-Wextra",
                          "-Werror", "-I", self.rdir, "-o", f"{self.rdir}/harness",
                          f"{self.rdir}/tab5-textcolor-harness.c",
                          f"{self.rdir}/tab5_textcolor.c")
        return rc == 0, out

    def put(self, name, data):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as f:
            f.write(data)

    def sync(self):
        subprocess.run(["docker", "cp", self.tmp + "/.", f"{self.ct}:{self.rdir}/"],
                       check=True, capture_output=True)

    def run(self, name, args):
        return self._x(f"{self.rdir}/harness", f"{self.rdir}/{name}", args)

    def sh(self, cmd):
        return self._x("sh", "-c", f"cd {self.rdir} && {cmd}")

    def clean(self):
        self._x("rm", "-rf", self.rdir)


def pick_runner(tmp):
    for cc in ("cc", "gcc"):
        if shutil.which(cc):
            return Native(shutil.which(cc), tmp)
    if not os.environ.get("CI") and shutil.which("docker"):
        r = subprocess.run(["docker", "ps", "--format", "{{.Names}}"],
                           capture_output=True, text=True)
        names = r.stdout.split()
        if r.returncode == 0 and names:
            return Docker(names[0], tmp)
    return None


def parse(out):
    res = {}
    for line in out.splitlines():
        k, _, v = line.partition("=")
        res[k] = v
    return res


def harness_checks(args, presets):
    with tempfile.TemporaryDirectory() as tmp:
        runner = pick_runner(tmp)
        if runner is None:
            if os.environ.get("CI"):
                bad("no C compiler for the harness")
            else:
                print("SKIP: harness (no native cc/gcc, no docker container)")
            return
        try:
            built, log = runner.build()
            if not built:
                bad(f"harness build failed:\n{log}")
                return
            ok("harness builds with -Wall -Wextra -Werror")
            all_cases = cases()
            for i, (_, img, *_rest) in enumerate(all_cases):
                runner.put(f"case{i}.img", img)
            if isinstance(runner, Docker):
                runner.sync()
            for i, (label, _, rc, name, preset) in enumerate(all_cases):
                code, out = runner.run(f"case{i}.img", args)
                res = parse(out)
                want_args = patched_args(args, palette(presets[preset])) if preset else args
                got = (res.get("rc"), bytes.fromhex(res.get("name", "")).decode("latin-1"),
                       res.get("patched"), res.get("args"))
                want = (str(rc), name, "1" if preset else "0", want_args)
                if code == 0 and got == want:
                    ok(label)
                else:
                    bad(f"{label}: exit {code}, got {got[:3]} want {want[:3]}"
                        + ("" if got[3] == want[3] else f"\n  args {got[3]}"))

            # Nothing is written when one of the three is missing or bent.
            half = args.replace(stock_param("blu"), "")
            code, out = runner.run("case0.img", half)
            res = parse(out)
            if res.get("patched") == "0" and res.get("args") == half:
                ok("no vt.default_blu: the bootargs stay untouched")
            else:
                bad(f"missing blu placeholder: {res}")
            bent = args.replace("vt.default_grn=0x00,", "vt.default_grn=0x0,0", 1)
            code, out = runner.run("case0.img", bent)
            res = parse(out)
            if res.get("patched") == "0" and res.get("args") == bent:
                ok("a bent value list: the bootargs stay untouched")
            else:
                bad(f"bent grn placeholder: {res}")
            longer = args.replace(stock_param("red"), stock_param("red") + ",0x00")
            code, out = runner.run("case0.img", longer)
            res = parse(out)
            if res.get("patched") == "0":
                ok("17 values: the bootargs stay untouched")
            else:
                bad(f"17-value red placeholder patched: {res}")

            real_image_check(runner, args, presets)
        finally:
            if isinstance(runner, Docker):
                runner.clean()


def real_image_check(runner, args, presets):
    """The boot FAT as post-build.sh makes it, config.txt then saved with
    a text_color= line by mcopy (a lowercase name: VFAT long name)."""
    rc, out = runner.sh("command -v mkfs.fat && command -v mcopy")
    if rc != 0:
        print("SKIP: real mkfs.fat image (no mkfs.fat/mcopy)")
        return
    runner.put("config.txt", FILLER * 40 + b"text_color=orange\r\n")
    if isinstance(runner, Docker):
        runner.sync()
    rc, out = runner.sh("rm -f real.img && mkfs.fat -C -F 12 -r 64 -n TAB5 "
                        "--invariant real.img 256 >/dev/null && "
                        "MTOOLS_SKIP_CHECK=1 mcopy -m -i real.img config.txt ::/")
    if rc != 0:
        bad(f"mkfs.fat/mcopy failed: {out}")
        return
    code, out = runner.run("real.img", args)
    res = parse(out)
    want = patched_args(args, palette(presets["orange"]))
    if res.get("rc") == "1" and res.get("patched") == "1" and res.get("args") == want:
        ok("a mkfs.fat + mcopy boot FAT: orange read and patched")
    else:
        bad(f"real image: {res}")


def main():
    shp, cp = shell_presets(), c_presets()
    args = static_checks(shp, cp)
    if args is not None and shp and shp == cp:
        harness_checks(args, shp)
    print(f"{checks - fails}/{checks} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
