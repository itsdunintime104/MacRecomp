"""Build recon/names.json: names for the stripped functions of a game's PE32 image.

Usage: python3 -I build_names.py <evidence.json> --binary <exe> [--startup-names <json>]
           [--analysis recon/analysis.json] [--out recon/names.json] [--saga <saga checkout>]

Run it from the game repo root: --analysis and --out default to recon/ there.

<evidence.json> comes from tools/ghidra/ExportNameEvidence.java (FunctionID matches and the
data addresses each function refers to). Sources, most trusted first:
- fid: Ghidra FunctionID. A single name from the Visual Studio 2005 libraries is "high";
  several 2005 names, or one name from another version only, is "medium"; else "low".
- string: a whole `Class::Method` string that only this function refers to ("high"), or a
  `{name string, function}` record in a data table of such records ("medium").
- rtti: MSVC RTTI. A vtable slot gets `Class::vfunc_NN` ("high"). A slot shared by classes
  that all derive from one of them is named after that base. A slot shared by unrelated
  classes (folded identical code) is "low". A function that stores one class's vtable and is
  no slot is `Class::ctor_or_dtor` ("low"). A slot that starts with the
  `test byte [esp+4], 1` of a deleting destructor is `Class::scalar_deleting_dtor` ("medium").
- startup-trace: hand names from the --startup-names file, a JSON object of
  {"0x<VA>": "<name>"} ("medium").
- saga: a string literal that occurs in one function here and in one function of the saga
  decompilation (LEGO Star Wars: The Complete Saga); "medium" when two literals agree.
Only function starts in the --analysis file are written.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
from collections import defaultdict
from pathlib import Path
from typing import Any, NamedTuple

UNDNAME = "/opt/homebrew/opt/llvm/bin/llvm-undname"
IMAGE_BASE = 0x400000

CONF_RANK = {"high": 0, "medium": 1, "low": 2}
SOURCE_RANK = {"fid": 0, "string": 1, "rtti": 2, "startup-trace": 3, "saga": 4}



class Name(NamedTuple):
    name: str
    source: str
    confidence: str
    evidence: str


class Pe:
    def __init__(self, data: bytes) -> None:
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        count = struct.unpack_from("<H", data, pe + 6)[0]
        opt = struct.unpack_from("<H", data, pe + 20)[0]
        self.sections: dict[str, tuple[int, int, int]] = {}
        for i in range(count):
            o = pe + 24 + opt + i * 40
            name = data[o : o + 8].rstrip(b"\0").decode()
            vsize, rva, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
            self.sections[name] = (IMAGE_BASE + rva, min(vsize, rsize), rptr)

    def offset(self, va: int) -> int | None:
        for start, size, rptr in self.sections.values():
            if start <= va < start + size:
                return rptr + va - start
        return None

    def u32(self, va: int) -> int | None:
        o = self.offset(va)
        return None if o is None else struct.unpack_from("<I", self.data, o)[0]

    def in_section(self, va: int, name: str) -> bool:
        start, size, _ = self.sections[name]
        return start <= va < start + size

    def words(self, name: str) -> tuple[int, tuple[int, ...]]:
        start, size, rptr = self.sections[name]
        return start, struct.unpack_from("<%dI" % (size // 4), self.data, rptr)

    def cstr(self, va: int) -> str | None:
        """A printable NUL-terminated string of 4+ characters at `va`, or None."""
        o = self.offset(va)
        if o is None:
            return None
        end = self.data.find(b"\0", o, o + 512)
        raw = self.data[o:end] if end >= 0 else b""
        if len(raw) < 4 or any((b < 0x20 or b > 0x7E) and b not in (9, 10, 13) for b in raw):
            return None
        return raw.decode()


def undname(mangled: list[str], flags: list[str]) -> dict[str, str]:
    """Mangled -> demangled for the names llvm-undname accepts."""
    if not mangled:
        return {}
    out = subprocess.run(
        [UNDNAME, *flags], input="\n".join(mangled) + "\n", capture_output=True, text=True,
        check=True,
    ).stdout
    result: dict[str, str] = {}
    for block in out.split("\n\n"):
        lines = block.strip("\n").split("\n")
        if len(lines) == 2 and not lines[1].startswith("error:"):
            result[lines[0]] = lines[1]
    return result


def strip_params(name: str) -> str:
    """`A::f(int) const` -> `A::f`; tag words dropped."""
    name = re.sub(r"\b(?:class|struct|union|enum) ", "", name).strip()
    if name.endswith(" const"):
        name = name[: -len(" const")]
    if name.endswith(")"):
        depth = 0
        for i in range(len(name) - 1, -1, -1):
            depth += {")": 1, "(": -1}.get(name[i], 0)
            if depth == 0:
                if i > 0 and not name[:i].endswith("operator"):
                    name = name[:i]
                break
    return name


def fid_names(evidence: dict[str, Any]) -> dict[int, Name]:
    rows: list[tuple[int, list[str], str, str]] = []
    for row in evidence["fid"]:
        matches = row["matches"]
        vs2005 = [m for m in matches if m["ver"] == "2005"]
        pool = vs2005 or matches
        names = sorted({m["name"] for m in pool})
        if vs2005:
            conf = "high" if len(names) == 1 else "medium"
        else:
            conf = "medium" if len(names) == 1 else "low"
        libs = sorted({f"VS{m['ver']} {m['variant']}" for m in pool})
        rows.append((row["addr"], names, conf, ", ".join(libs)))
    demangled = undname(
        sorted({n for _, names, _, _ in rows for n in names if n.startswith("?")}),
        ["--no-calling-convention", "--no-return-type", "--no-access-specifier",
         "--no-member-type", "--no-variable-type"],
    )
    result: dict[int, Name] = {}
    for addr, names, conf, libs in rows:
        name = strip_params(demangled.get(names[0], names[0]))
        note = f"{libs}; {len(names)} candidate names" if len(names) > 1 else libs
        result[addr] = Name(name, "fid", conf, note)
    return result


class Rtti(NamedTuple):
    slots: dict[int, list[tuple[str, int, int, int]]]  # func -> (class, sub-object offset, slot, vtable)
    vtables: dict[int, str]  # vtable start -> class
    bases: dict[str, set[str]]  # class -> its base classes


def read_rtti(pe: Pe, starts: set[int]) -> Rtti:
    descriptors: dict[int, str] = {}
    for sec in (".data", ".rdata"):
        start, size, rptr = pe.sections[sec]
        blob = pe.data[rptr : rptr + size]
        for tag in (b".?AV", b".?AU"):
            pos = blob.find(tag)
            while pos >= 0:
                end = blob.find(b"\0", pos)
                descriptors[start + pos - 8] = blob[pos:end].decode("latin-1")
                pos = blob.find(tag, end)
    plain = undname([f"??_R0{m[1:]}@8" for m in descriptors.values()], [])
    class_of = {
        td: strip_params(plain.get(f"??_R0{m[1:]}@8", m).replace(" `RTTI Type Descriptor'", ""))
        for td, m in descriptors.items()
    }
    rstart, words = pe.words(".rdata")
    locators: dict[int, tuple[str, int]] = {}
    bases: dict[str, set[str]] = defaultdict(set)
    for i in range(len(words) - 4):
        if words[i] == 0 and words[i + 3] in class_of and pe.in_section(words[i + 4], ".rdata"):
            cls = class_of[words[i + 3]]
            locators[rstart + 4 * i] = (cls, words[i + 1])
            count, array = pe.u32(words[i + 4] + 8), pe.u32(words[i + 4] + 12)
            for k in range(min(count or 0, 64)):
                bcd = pe.u32(array + 4 * k) if array else None
                td = pe.u32(bcd) if bcd else None
                if td in class_of and class_of[td] != cls:
                    bases[cls].add(class_of[td])
    slots: dict[int, list[tuple[str, int, int, int]]] = defaultdict(list)
    vtables: dict[int, str] = {}
    for i, word in enumerate(words):
        if word not in locators:
            continue
        cls, sub = locators[word]
        vt = rstart + 4 * (i + 1)
        vtables[vt] = cls
        for k in range(i + 1, len(words)):
            target = words[k]
            if not pe.in_section(target, ".text"):
                break
            if target in starts:
                slots[target].append((cls, sub, k - i - 1, vt))
    return Rtti(slots, vtables, bases)


def rtti_names(pe: Pe, rtti: Rtti, evidence: dict[str, Any], starts: set[int]) -> dict[int, Name]:
    result: dict[int, Name] = {}
    for func, uses in rtti.slots.items():
        classes = {u[0] for u in uses}
        base = next(
            (c for c in sorted(classes) if all(c == o or c in rtti.bases[o] for o in classes)), None
        )
        owner = base or sorted(classes)[0]
        cls, sub, slot, vt = min(u for u in uses if u[0] == owner)
        member = f"vfunc_{slot:02d}" if sub == 0 else f"vfunc_{sub:X}_{slot:02d}"
        conf = "high"
        o = pe.offset(func)
        head = pe.data[o : o + 48] if o is not None else b""
        if base is None:
            conf = "low"
            member += f"_shared{len(classes)}"
        elif b"\xf6\x44\x24\x04\x01" in head or b"\xf6\x45\x08\x01" in head:
            member, conf = "scalar_deleting_dtor", "medium"
        result[func] = Name(f"{cls}::{member}", "rtti", conf, f"vtable 0x{vt:08X} slot {slot}")
    for row in evidence["functions"]:
        func = row["addr"]
        if func in rtti.slots or func not in starts:
            continue
        owners = {rtti.vtables[r] for r in row["refs"] if r in rtti.vtables}
        if len(owners) == 1:
            cls = owners.pop()
            result[func] = Name(f"{cls}::ctor_or_dtor", "rtti", "low", "stores the vtable")
    return result


MEMBER_STRING_RE = re.compile(r"[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)+(?:\(\))?\Z")
DATA_MEMBER_RE = re.compile(r"(?:[ms]_|sm_|[ms][A-Z])")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{2,63}\Z")


def string_names(pe: Pe, evidence: dict[str, Any], starts: set[int]) -> dict[int, Name]:
    result: dict[int, Name] = {}
    users: dict[int, set[int]] = defaultdict(set)
    texts: dict[int, str] = {}
    for row in evidence["functions"]:
        for ref in row["refs"]:
            text = texts.get(ref) or pe.cstr(ref)
            if text and MEMBER_STRING_RE.match(text):
                member = text.removesuffix("()").rpartition("::")[2]
                if not DATA_MEMBER_RE.match(member):
                    texts[ref] = text
                    users[ref].add(row["addr"])
    per_func: dict[int, set[int]] = defaultdict(set)
    for ref, funcs in users.items():
        for func in funcs:
            per_func[func].add(ref)
    for ref, funcs in users.items():
        func = next(iter(funcs))
        if len(funcs) == 1 and len(per_func[func]) == 1 and func in starts:
            name = texts[ref].removesuffix("()")
            result[func] = Name(name, "string", "high", f"string 0x{ref:08X}")

    # Tables of {name string, function} records: a record is accepted when the same pair
    # shape repeats at a fixed stride, so the pairing is the table's layout and not chance.
    pairs: dict[int, tuple[str, int]] = {}
    for sec in (".rdata", ".data"):
        start, words = pe.words(sec)
        for i in range(len(words) - 1):
            if words[i + 1] in starts and (text := pe.cstr(words[i])) and IDENT_RE.match(text):
                pairs[start + 4 * i] = (text, words[i + 1])
    named: dict[int, set[str]] = defaultdict(set)
    where: dict[int, int] = {}
    for pos, (text, func) in pairs.items():
        for stride in range(8, 68, 4):
            near = (pos - stride in pairs) + (pos + stride in pairs)
            if near == 2 or (near == 1 and (pos - 2 * stride in pairs or pos + 2 * stride in pairs)):
                named[func].add(text)
                where.setdefault(func, pos)
                break
    for func, texts_for in named.items():
        if len(texts_for) == 1 and func not in result:
            text = next(iter(texts_for))
            result[func] = Name(text, "string", "medium", f"table record 0x{where[func]:08X}")
    return result


SAGA_DEF_RE = re.compile(
    r"^(?!(?:return|else|case|new|delete|sizeof|throw|if|while|for|switch|do)\b)"
    r"[A-Za-z_][\w:\s\*&<>,]*?(~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*)\s*\([^;]*$"
)
LITERAL_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
MIN_LITERAL = 8
ESCAPE_RE = re.compile(r"\\(.)")
ESCAPES = {"n": "\n", "t": "\t", "r": "\r"}


def saga_names(saga: Path, pe: Pe, evidence: dict[str, Any], starts: set[int]) -> dict[int, Name]:
    """Match string literals that occur in exactly one saga function and one function here."""
    saga_users: dict[str, set[str]] = defaultdict(set)
    for path in sorted((saga / "src").rglob("*")):
        if path.suffix not in (".c", ".cpp", ".cc"):
            continue
        current: str | None = None
        for line in path.read_text(encoding="latin-1").splitlines():
            match = SAGA_DEF_RE.match(line)
            if match:
                current = match[1]
            elif current and line.startswith("}"):
                current = None
            elif current:
                for raw in LITERAL_RE.findall(line):
                    text = ESCAPE_RE.sub(lambda m: ESCAPES.get(m[1], m[1]), raw)
                    if len(text) >= MIN_LITERAL:
                        saga_users[text].add(current)
    our_users: dict[str, set[int]] = defaultdict(set)
    for row in evidence["functions"]:
        for ref in row["refs"]:
            text = pe.cstr(ref)
            if text and len(text) >= MIN_LITERAL and text in saga_users:
                our_users[text].add(row["addr"])
    votes: dict[int, dict[str, list[str]]] = defaultdict(lambda: defaultdict(list))
    for text, funcs in our_users.items():
        if len(funcs) == 1 and len(saga_users[text]) == 1:
            votes[next(iter(funcs))][next(iter(saga_users[text]))].append(text)
    result: dict[int, Name] = {}
    for func, by_name in votes.items():
        if len(by_name) != 1 or func not in starts:
            continue
        name, texts = next(iter(by_name.items()))
        conf = "medium" if len(texts) >= 2 else "low"
        result[func] = Name(name, "saga", conf, f"{len(texts)} literal(s): {texts[0][:40]!r}")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description="Build recon/names.json.")
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--binary", type=Path, required=True, help="the game's PE32 image")
    parser.add_argument("--startup-names", type=Path, help='JSON {"0x<VA>": "<name>"} of hand names')
    parser.add_argument("--analysis", type=Path, default=Path("recon/analysis.json"))
    parser.add_argument("--out", type=Path, default=Path("recon/names.json"))
    parser.add_argument("--saga", type=Path)
    args = parser.parse_args()
    evidence: dict[str, Any] = json.loads(args.evidence.read_text())
    starts = {f["addr"] for f in json.loads(args.analysis.read_text())["functions"]}
    pe = Pe(args.binary.read_bytes())
    startup: dict[int, str] = {}
    if args.startup_names:
        startup = {int(va, 16): name for va, name in json.loads(args.startup_names.read_text()).items()}
    rtti = read_rtti(pe, starts)
    sources: list[dict[int, Name]] = [
        fid_names(evidence),
        string_names(pe, evidence, starts),
        rtti_names(pe, rtti, evidence, starts),
        {a: Name(n, "startup-trace", "medium", str(args.startup_names)) for a, n in startup.items()},
    ]
    if args.saga:
        sources.append(saga_names(args.saga, pe, evidence, starts))
    best: dict[int, Name] = {}
    for found in sources:
        for addr, cand in found.items():
            if addr not in starts:
                continue
            old = best.get(addr)
            key = (CONF_RANK[cand.confidence], SOURCE_RANK[cand.source])
            if old is None or key < (CONF_RANK[old.confidence], SOURCE_RANK[old.source]):
                best[addr] = cand
    dropped = sorted(a for a in startup if a not in starts)
    out = {
        f"0x{addr:08X}": {"name": n.name, "source": n.source, "confidence": n.confidence,
                          "evidence": n.evidence}
        for addr, n in sorted(best.items())
    }
    args.out.write_text(json.dumps(out, indent=1) + "\n")
    counts: dict[tuple[str, str], int] = defaultdict(int)
    for n in best.values():
        counts[(n.source, n.confidence)] += 1
    print(f"{len(out)} names -> {args.out}")
    for (source, conf), count in sorted(counts.items(), key=lambda kv: (SOURCE_RANK[kv[0][0]], CONF_RANK[kv[0][1]])):
        print(f"  {source:14} {conf:6} {count}")
    print(f"vtables {len(rtti.vtables)}, classes with vtables {len(set(rtti.vtables.values()))}")
    print("startup-trace addresses that are not function starts:", [hex(a) for a in dropped])
    print("functions with candidates per source:", [len(s) for s in sources])


if __name__ == "__main__":
    main()
