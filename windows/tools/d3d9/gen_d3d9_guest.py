"""Generate the guest-facing Direct3D 9 COM layer.

Usage (from the repository root):
    python -I tools/d3d9/gen_d3d9_guest.py \
        --dxvk /path/to/dxvk          # a DXVK checkout (its include/native)
        [--mingw-include DIR]         # i686 MinGW include dir (guest truth)
        [--extra-exports FILE.json]   # more win32_exports_d3d[] entries
        [--clang clang]

Outputs runtime/include/d3d9_guest.h, runtime/src/gen_d3d9_guest.c and
notes/d3d9_marshal.md. Do not edit them by hand; rerun this script.

Guest truth: the i686 MinGW d3d9.h, read through clang's JSON AST (method
order, parameter names and types), plus compile-only probes for the i686
layout of every struct the methods take. Host truth: DXVK Native's headers,
probed the same way for arm64. A probe value is read back from the AST as the
length of an array `char d3d9probe_N[EXPR + 1]`, so nothing is run.

--extra-exports takes a JSON list of {"dll", "name", "ordinal", "fn"}; each
entry is appended to win32_exports_d3d[] and fn is declared `void fn(void)`.
The D3DX slice adds its entries this way.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from enum import StrEnum
from pathlib import Path
from typing import NoReturn

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MINGW = "/opt/homebrew/Cellar/mingw-w64/13.0.0_2/toolchain-i686/i686-w64-mingw32/include"


class ArgClass(StrEnum):
    SCALAR32 = "scalar32"
    FLOAT = "float"
    HWND = "hwnd"
    HANDLE_NULL = "handle_null"
    IFACE_IN = "iface_in"
    IFACE_OUT = "iface_out"
    IID_OUT = "iid_out"
    DATA_PTR = "data_ptr"
    SCALAR_PTR = "scalar_ptr"
    STRUCT_PASS = "struct_pass"
    STRUCT_CONV = "struct_conv"
    SPECIAL = "special"


class Kind(StrEnum):
    GENERATED = "generated"
    SPECIAL = "special"
    MANUAL = "manual"
    FALLBACK = "fallback"


class RetClass(StrEnum):
    EAX = "eax"
    FLOAT = "float"
    VOID = "void"
    ZERO = "zero"


class Verdict(StrEnum):
    PASS = "pass"
    CONV = "conv"
    UNUSABLE = "unusable"


class Direction(StrEnum):
    IN = "in"
    OUT = "out"
    INOUT = "inout"


class TypeKind(StrEnum):
    BUILTIN = "builtin"
    ENUM = "enum"
    RECORD = "record"
    INTERFACE = "interface"
    VOID = "void"
    UNKNOWN = "unknown"


INTERFACES = [
    "IDirect3D9",
    "IDirect3DDevice9",
    "IDirect3DSwapChain9",
    "IDirect3DResource9",
    "IDirect3DBaseTexture9",
    "IDirect3DTexture9",
    "IDirect3DCubeTexture9",
    "IDirect3DVolumeTexture9",
    "IDirect3DSurface9",
    "IDirect3DVolume9",
    "IDirect3DVertexBuffer9",
    "IDirect3DIndexBuffer9",
    "IDirect3DVertexDeclaration9",
    "IDirect3DVertexShader9",
    "IDirect3DPixelShader9",
    "IDirect3DStateBlock9",
    "IDirect3DQuery9",
]

# Decision 6: host memory the guest must write; hand-written in
# runtime/src/d3d9_guest_manual.c.
MANUAL = {
    ("IDirect3DSurface9", "LockRect"),
    ("IDirect3DSurface9", "UnlockRect"),
    ("IDirect3DTexture9", "LockRect"),
    ("IDirect3DTexture9", "UnlockRect"),
    ("IDirect3DCubeTexture9", "LockRect"),
    ("IDirect3DCubeTexture9", "UnlockRect"),
    ("IDirect3DVolume9", "LockBox"),
    ("IDirect3DVolume9", "UnlockBox"),
    ("IDirect3DVolumeTexture9", "LockBox"),
    ("IDirect3DVolumeTexture9", "UnlockBox"),
    ("IDirect3DVertexBuffer9", "Lock"),
    ("IDirect3DVertexBuffer9", "Unlock"),
    ("IDirect3DIndexBuffer9", "Lock"),
    ("IDirect3DIndexBuffer9", "Unlock"),
    ("IDirect3DSurface9", "GetDC"),
    ("IDirect3DSurface9", "ReleaseDC"),
}

# Hand-written steps a generated thunk runs before its host call, after the
# argument copies in (d3d9_guest_manual.c); a1..aN, hN and pN are in scope.
# The game asks for exclusive fullscreen; the host gets a window instead.
BEFORE_CALL = {
    ("IDirect3D9", "CreateDevice"): "d3d9g_mode_keep k5; d3d9g_windowed_in(p5, &k5, d3d9g_hwnd_in(a3));",
    ("IDirect3DDevice9", "Reset"): "d3d9g_mode_keep k1; d3d9g_windowed_in(p1, &k1, NULL);",
    ("IDirect3DDevice9", "CreateAdditionalSwapChain"): "d3d9g_mode_keep k1; d3d9g_windowed_in(p1, &k1, NULL);",
    ("IDirect3DDevice9", "Present"): "d3d9_hud_present(self); d3d9g_dump_frame(self);",
}

# Hand-written state changes a generated thunk makes after its host call
# (d3d9_guest_manual.c); r and a1..aN are in scope. Windows D3D9 sets the
# calling thread's x87 control word in CreateDevice; DXVK cannot reach the
# guest's, so the thunk does it. The d3d9g_trace calls feed HP_TRACE_D3D9.
AFTER_CALL = {
    ("IDirect3D9", "CreateDevice"): "d3d9g_windowed_out(a5, &k5); d3d9g_after_create_device((uint32_t)r, a4);",
    ("IDirect3DDevice9", "Reset"): "d3d9g_windowed_out(a1, &k1); d3d9g_trace(D3D9G_TR_RESET, (uint32_t)r);",
    ("IDirect3DDevice9", "CreateAdditionalSwapChain"): "d3d9g_windowed_out(a1, &k1);",
    ("IDirect3D9", "GetAdapterModeCount"): "r = (UINT)d3d9g_mode_count((uint32_t)r);",
    ("IDirect3D9", "EnumAdapterModes"): "r = (HRESULT)d3d9g_enum_mode((uint32_t)r, a2, a3, a4);",
    ("IDirect3D9", "GetAdapterDisplayMode"): "d3d9g_display_mode((uint32_t)r, a2);",
    ("IDirect3DDevice9", "Present"): "d3d9g_trace(D3D9G_TR_PRESENT, (uint32_t)r);",
    ("IDirect3DSwapChain9", "Present"): "d3d9g_trace(D3D9G_TR_PRESENT, (uint32_t)r);",
    ("IDirect3DDevice9", "EndScene"): "d3d9g_trace(D3D9G_TR_END_SCENE, (uint32_t)r);",
    ("IDirect3DDevice9", "DrawPrimitive"): "d3d9g_trace(D3D9G_TR_DRAW, (uint32_t)r);",
    ("IDirect3DDevice9", "DrawIndexedPrimitive"): "d3d9g_trace(D3D9G_TR_DRAW, (uint32_t)r);",
    ("IDirect3DDevice9", "DrawPrimitiveUP"): "d3d9g_trace(D3D9G_TR_DRAW, (uint32_t)r);",
    ("IDirect3DDevice9", "DrawIndexedPrimitiveUP"): "d3d9g_trace(D3D9G_TR_DRAW, (uint32_t)r);",
    ("IDirect3DDevice9", "SetRenderTarget"): "d3d9g_trace(D3D9G_TR_SET_RENDER_TARGET, (uint32_t)r);",
    ("IDirect3DDevice9", "TestCooperativeLevel"): "d3d9g_trace(D3D9G_TR_TEST_COOPERATIVE_LEVEL, (uint32_t)r);",
}

# Structs the manual lock code reads or writes in guest memory; the header
# gets their guest offsets.
MANUAL_STRUCTS = ["D3DLOCKED_RECT", "D3DLOCKED_BOX", "D3DPRESENT_PARAMETERS"]

BUILTINS = {
    "char": 1, "signed char": 1, "unsigned char": 1, "short": 2, "unsigned short": 2,
    "int": 4, "unsigned int": 4, "long": 4, "unsigned long": 4, "long long": 8,
    "unsigned long long": 8, "float": 4, "double": 8, "wchar_t": 2, "_Bool": 1, "bool": 1,
}
HANDLE_NAMES = ("HWND", "HANDLE", "HMONITOR", "HDC")

ARG_CLASSES: dict[ArgClass, str] = {
    ArgClass.SCALAR32: "32-bit integer, enum or BOOL: W32_ARG(i) as is",
    ArgClass.FLOAT: "float on the stack: the 32 bits reinterpreted",
    ArgClass.HWND: "HWND: d3d9g_hwnd_in() (calls the UI slice's d3d9g_hwnd)",
    ArgClass.HANDLE_NULL: "HANDLE *: must be NULL, else abort with an UNSUPPORTED line",
    ArgClass.IFACE_IN: "input interface pointer: NULL stays NULL, else com_guest_self",
    ArgClass.IFACE_OUT: "IDirect3DXxx9 ** out: host object wrapped as a guest object of the declared type",
    ArgClass.DATA_PTR: "void * / const void * data: guest_ptr",
    ArgClass.SCALAR_PTR: "pointer to 32-bit scalars (DWORD *, float *, BOOL *, ...): guest_ptr",
    ArgClass.STRUCT_PASS: "pointer to a struct with the same guest and host layout: guest_ptr",
    ArgClass.STRUCT_CONV: "pointer to a struct with a different layout: copied in and/or out",
    ArgClass.IID_OUT: "void ** out next to a REFIID (GetContainer): wrapped by IID, as QueryInterface",
    ArgClass.SPECIAL: "QueryInterface's REFIID and void ** (IID table)",
}


def die(msg: str) -> NoReturn:
    sys.exit(f"gen_d3d9_guest: {msg}")


# ---- clang ------------------------------------------------------------------

@dataclass
class Target:
    name: str
    args: list[str]


def clang_run(clang: str, tgt: Target, src: str, lang: str, extra: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [clang, *tgt.args, "-x", lang, "-fsyntax-only", "-ferror-limit=0", "-w", *extra, "-"],
        input=src, capture_output=True, text=True, check=False)


def json_objects(text: str) -> list[dict]:
    """The concatenated JSON objects of an -ast-dump-filter run."""
    dec = json.JSONDecoder()
    out, i = [], 0
    while True:
        j = text.find("{", i)
        if j < 0:
            return out
        obj, i = dec.raw_decode(text, j)
        out.append(obj)


def ast_dump(clang: str, tgt: Target, src: str, lang: str, filt: str | None = None) -> list[dict]:
    extra = ["-Xclang", "-ast-dump=json"]
    if filt:
        extra += ["-Xclang", f"-ast-dump-filter={filt}"]
    r = clang_run(clang, tgt, src, lang, extra)
    if r.returncode != 0:
        die(f"clang failed for {tgt.name}:\n{r.stderr[:4000]}")
    if filt:
        return json_objects(r.stdout)
    return json.loads(r.stdout)["inner"]


def probe(clang: str, tgt: Target, prelude: str, items: list[tuple[str, str]]) -> dict[str, int | None]:
    """Evaluate each (key, C constant expression) at compile time on tgt.
    An item whose line does not compile (a type or field missing on that
    target) yields None."""
    def source(keep: list[int]) -> tuple[str, dict[int, int]]:
        lines = prelude.rstrip("\n").split("\n") + ["#include <stddef.h>"]
        line_of: dict[int, int] = {}
        for k in keep:
            lines.append(f"char d3d9probe_{k}[({items[k][1]}) + 1];")
            line_of[len(lines)] = k
        return "\n".join(lines) + "\n", line_of

    keep = list(range(len(items)))
    src, line_of = source(keep)
    r = clang_run(clang, tgt, src, "c", [])
    if r.returncode != 0:
        bad = {line_of.get(int(m.group(1))) for m in re.finditer(r"^<stdin>:(\d+):\d+: error", r.stderr, re.M)}
        if None in bad:
            die(f"probe prelude failed for {tgt.name}:\n{r.stderr[:4000]}")
        keep = [k for k in keep if k not in bad]
        src, line_of = source(keep)
    objs = ast_dump(clang, tgt, src, "c", "d3d9probe_")
    vals: dict[str, int | None] = {key: None for key, _ in items}
    for o in objs:
        m = re.fullmatch(r"d3d9probe_(\d+)", o.get("name", ""))
        if o.get("kind") != "VarDecl" or not m:
            continue
        n = re.fullmatch(r"char\[(\d+)\]", o["type"]["qualType"])
        if not n:
            die(f"probe {o['name']}: unexpected type {o['type']['qualType']}")
        vals[items[int(m.group(1))][0]] = int(n.group(1)) - 1
    return vals


# ---- guest types -----------------------------------------------------------

@dataclass
class TypeInfo:
    spelling: str  # as declared, e.g. "const RECT *"
    base: str  # declared base name, e.g. "RECT"
    const: bool  # the base is const
    ptr: int  # declared pointer/reference depth
    is_ref: bool
    array: str  # e.g. "[4][4]"
    kind: TypeKind
    canon: str  # builtin name, record name, enum name
    chain: list[str] = field(default_factory=list)  # typedef names passed
    tptr: int = 0  # pointers added by typedefs (HWND -> struct HWND__ *)
    anon: bool = False  # an unnamed record type


SPELL_RE = re.compile(r"^(?P<body>.*?)\s*(?P<arr>(\[\d*\])*)$")


def split_spelling(s: str) -> tuple[str, bool, int, bool, str]:
    m = SPELL_RE.match(s.strip())
    assert m
    body, arr = m.group("body").strip(), m.group("arr")
    ptr, is_ref = 0, False
    while True:
        body = re.sub(r"\s*\bconst\s*$", "", body)
        if body.endswith("*"):
            ptr, body = ptr + 1, body[:-1].rstrip()
        elif body.endswith("&"):
            ptr, is_ref, body = ptr + 1, True, body[:-1].rstrip()
        else:
            break
    const = bool(re.match(r"^const\b", body)) or bool(re.search(r"\bconst$", body))
    body = re.sub(r"\bconst\b", "", body).strip()
    body = re.sub(r"\s+", " ", body)
    return body, const, ptr, is_ref, arr


def top_level(tu: list[dict]) -> list[dict]:
    """Top-level declarations, including those inside extern "C" blocks."""
    out: list[dict] = []
    for n in tu:
        if n.get("kind") == "LinkageSpecDecl":
            out += top_level(n.get("inner", []))
        else:
            out.append(n)
    return out


class GuestTypes:
    def __init__(self, tu: list[dict]):
        self.typedefs: dict[str, str] = {}
        self.records: dict[str, dict] = {}
        self.enums: set[str] = set()
        self.ifaces: dict[str, dict] = {}
        for n in top_level(tu):
            k, name = n.get("kind"), n.get("name")
            if not name:
                continue
            if k == "TypedefDecl":
                self.typedefs[name] = n["type"]["qualType"]
            elif k == "EnumDecl":
                self.enums.add(name)
            elif k in ("CXXRecordDecl", "RecordDecl") and n.get("completeDefinition"):
                if n.get("bases") is not None or name == "IUnknown":
                    self.ifaces[name] = n
                self.records[name] = n

    def resolve(self, spelling: str) -> TypeInfo:
        base, const, ptr, is_ref, arr = split_spelling(spelling)
        t = TypeInfo(spelling, base, const, ptr, is_ref, arr, TypeKind.UNKNOWN, base)
        cur = base
        for _ in range(32):
            cur = re.sub(r"^(struct|union|enum|class) ", "", cur)
            if "(anonymous" in cur or "(unnamed" in cur:
                t.kind, t.canon, t.anon = TypeKind.RECORD, cur, True
                return t
            if cur in BUILTINS:
                t.kind, t.canon = TypeKind.BUILTIN, cur
                return t
            if cur == "void":
                t.kind, t.canon = TypeKind.VOID, cur
                return t
            if cur in self.ifaces and (cur in INTERFACES or cur == "IUnknown"):
                t.kind, t.canon = TypeKind.INTERFACE, cur
                return t
            if cur in self.typedefs:
                t.chain.append(cur)
                b2, _c2, p2, _r2, a2 = split_spelling(self.typedefs[cur])
                t.tptr += p2
                if a2:
                    t.array = a2 + t.array
                cur = b2
                continue
            if cur in self.enums:
                t.kind, t.canon = TypeKind.ENUM, cur
                return t
            if cur in self.records:
                t.kind, t.canon = TypeKind.RECORD, cur
                return t
            t.canon = cur
            return t
        die(f"typedef loop at {spelling}")

    def handle_name(self, t: TypeInfo) -> str | None:
        for h in HANDLE_NAMES:
            if h in t.chain:
                return h
        return None


# ---- interfaces ------------------------------------------------------------

@dataclass
class Param:
    name: str
    t: TypeInfo
    cls: ArgClass | None = None
    struct: str = ""  # struct key for struct_* classes
    direction: Direction | None = None  # for struct_conv
    note: str = ""


@dataclass
class Method:
    iface: str
    name: str
    index: int
    ret: str
    ret_t: TypeInfo
    params: list[Param]
    mangled_bytes: int
    kind: Kind | None = None
    reason: str = ""
    ret_cls: RetClass | None = None

    @property
    def slots(self) -> int:
        return 1 + len(self.params)

    @property
    def cname(self) -> str:
        return f"d3d9g_{self.iface}_{self.name}"


def own_methods(types: GuestTypes, iface: str) -> list[dict]:
    node = types.ifaces[iface]
    return [x for x in node.get("inner", []) if x.get("kind") == "CXXMethodDecl" and x.get("virtual")
            and not x.get("isImplicit")]


def vtable(types: GuestTypes, iface: str) -> list[tuple[str, dict]]:
    node = types.ifaces.get(iface)
    if node is None:
        die(f"interface {iface} not in d3d9.h")
    bases = node.get("bases") or []
    order: list[tuple[str, dict]] = []
    if bases:
        if len(bases) != 1:
            die(f"{iface}: multiple bases")
        order = vtable(types, bases[0]["type"]["qualType"])
    names = [n for n, _ in order]
    for m in own_methods(types, iface):
        if m["name"] in names:
            order[names.index(m["name"])] = (m["name"], m)
        else:
            order.append((m["name"], m))
            names.append(m["name"])
    return order


def build_methods(types: GuestTypes) -> dict[str, list[Method]]:
    out: dict[str, list[Method]] = {}
    for iface in INTERFACES:
        ms = []
        for idx, (name, node) in enumerate(vtable(types, iface)):
            qt = node["type"]["qualType"]
            ret = qt[: qt.index("(")].strip()
            params = [Param(p.get("name") or f"arg{i + 1}", types.resolve(p["type"]["qualType"]))
                      for i, p in enumerate(x for x in node.get("inner", []) if x.get("kind") == "ParmVarDecl")]
            mm = re.search(r"@(\d+)$", node.get("mangledName", ""))
            if not mm:
                die(f"{iface}::{name}: no stdcall byte count in the mangled name")
            ms.append(Method(iface, name, idx, ret, types.resolve(ret), params, int(mm.group(1))))
        out[iface] = ms
    return out


# ---- structs ---------------------------------------------------------------

@dataclass
class Leaf:
    path: str
    t: TypeInfo


@dataclass
class Struct:
    key: str  # canonical record name
    spell: str  # C spelling valid on both sides, e.g. "RECT"
    leaves: list[Leaf]
    guest: dict[str, int | None] = field(default_factory=dict)
    host: dict[str, int | None] = field(default_factory=dict)
    verdict: Verdict = Verdict.UNUSABLE  # set by judge()
    reason: str = ""
    users: list[str] = field(default_factory=list)


def flatten(types: GuestTypes, rec: dict, prefix: str, depth: int = 0) -> list[Leaf]:
    if depth > 8:
        die("struct nesting too deep")
    leaves: list[Leaf] = []
    last_anon: dict | None = None
    for x in rec.get("inner", []):
        k = x.get("kind")
        if k in ("CXXRecordDecl", "RecordDecl"):
            if not x.get("name") and x.get("completeDefinition"):
                last_anon = x
            continue
        if k != "FieldDecl":
            continue
        qt = x["type"]["qualType"]
        name = x.get("name")
        t = types.resolve(qt)
        if t.anon:
            if last_anon is None:
                die(f"unnamed record field without a definition in {rec.get('name')}")
            leaves += flatten(types, last_anon, prefix + (f"{name}." if name else ""), depth + 1)
            continue
        if t.kind == TypeKind.RECORD and t.ptr == 0 and t.tptr == 0 and not t.array:
            leaves += flatten(types, types.records[t.canon], f"{prefix}{name}.", depth + 1)
            continue
        leaves.append(Leaf(prefix + str(name), t))
    return leaves


def struct_items(s: Struct) -> list[tuple[str, str]]:
    items = [(f"{s.key}|size", f"sizeof({s.spell})"), (f"{s.key}|align", f"_Alignof({s.spell})")]
    for lf in s.leaves:
        items.append((f"{s.key}|off|{lf.path}", f"offsetof({s.spell}, {lf.path})"))
        items.append((f"{s.key}|lsize|{lf.path}", f"sizeof((({s.spell} *)0)->{lf.path})"))
    return items


def judge(types: GuestTypes, s: Struct) -> None:
    g, h = s.guest, s.host
    if h.get("size") is None:
        s.verdict, s.reason = Verdict.UNUSABLE, "type missing from the host headers"
        return
    ptr_leaves = [lf for lf in s.leaves if lf.t.ptr + lf.t.tptr > 0]
    diffs = []
    if g["size"] != h["size"] or g["align"] != h["align"]:
        diffs.append(f"size/align {g['size']}/{g['align']} guest vs {h['size']}/{h['align']} host")
    missing = [lf.path for lf in s.leaves if h.get(f"off|{lf.path}") is None]
    moved = [lf.path for lf in s.leaves if lf.path not in missing
             and (g[f"off|{lf.path}"] != h[f"off|{lf.path}"] or g[f"lsize|{lf.path}"] != h[f"lsize|{lf.path}"])]
    if missing:
        s.verdict, s.reason = Verdict.UNUSABLE, f"fields missing on the host: {', '.join(missing)}"
        return
    if not diffs and not moved and not ptr_leaves:
        s.verdict = Verdict.PASS
        s.reason = f"{len(s.leaves)} fields, same offsets and sizes, no pointer fields"
        return
    if moved:
        diffs.append(f"fields at other offsets or sizes: {', '.join(moved)}")
    if ptr_leaves:
        diffs.append(f"pointer fields: {', '.join(lf.path for lf in ptr_leaves)}")
    bad = [lf.path for lf in s.leaves if lf.t.ptr + lf.t.tptr > 0 and types.handle_name(lf.t) != "HWND"]
    bad += [lf.path for lf in s.leaves if lf.t.ptr + lf.t.tptr == 0
            and g[f"lsize|{lf.path}"] != h[f"lsize|{lf.path}"]]
    if bad:
        s.verdict = Verdict.UNUSABLE
        s.reason = "; ".join(diffs) + f"; no field conversion for: {', '.join(bad)}"
    else:
        s.verdict, s.reason = Verdict.CONV, "; ".join(diffs)


# ---- classification --------------------------------------------------------

def classify(types: GuestTypes, methods: dict[str, list[Method]], structs: dict[str, Struct],
             guest_sizes: dict[str, int | None]) -> None:
    for ms in methods.values():
        for m in ms:
            slot_bytes = 0
            for p in m.params:
                t = p.t
                if t.ptr > 0:
                    slot_bytes += 4
                else:
                    sz = guest_sizes.get(t.spelling)
                    if sz is None:
                        die(f"{m.iface}::{m.name}: no guest size for {t.spelling}")
                    slot_bytes += (sz + 3) // 4 * 4
            if slot_bytes + 4 != m.mangled_bytes:
                die(f"{m.iface}::{m.name}: {slot_bytes + 4} argument bytes, mangled name says {m.mangled_bytes}")
            if any(guest_sizes.get(p.t.spelling, 4) != 4 for p in m.params if p.t.ptr == 0):
                m.kind, m.reason = Kind.FALLBACK, "an argument is not one 32-bit slot"
                continue

            # return value
            rt = m.ret_t
            if rt.kind == TypeKind.VOID and rt.ptr + rt.tptr == 0:
                m.ret_cls = RetClass.VOID
            elif rt.kind == TypeKind.BUILTIN and rt.canon == "float" and rt.ptr + rt.tptr == 0:
                m.ret_cls = RetClass.FLOAT
            elif rt.kind in (TypeKind.BUILTIN, TypeKind.ENUM) and rt.ptr + rt.tptr == 0 and guest_sizes.get(rt.spelling) == 4:
                m.ret_cls = RetClass.EAX
            else:
                m.ret_cls = RetClass.ZERO
                m.kind, m.reason = Kind.FALLBACK, f"returns {m.ret}"

            if m.name in ("QueryInterface", "AddRef", "Release"):
                m.kind = Kind.SPECIAL
                for p in m.params:
                    p.cls = ArgClass.SPECIAL
                continue
            if (m.iface, m.name) in MANUAL:
                m.kind = Kind.MANUAL
                continue
            if m.kind == Kind.FALLBACK:
                continue
            reasons = []
            for p in m.params:
                why = classify_param(types, m, p, structs)
                if why:
                    reasons.append(f"{p.name} ({p.t.spelling}): {why}")
            if reasons:
                m.kind, m.reason = Kind.FALLBACK, "; ".join(reasons)
            else:
                m.kind = Kind.GENERATED


def iid_param(m: Method) -> int | None:
    """1-based index of the method's REFIID/REFGUID argument, if any."""
    for i, p in enumerate(m.params, 1):
        if p.t.is_ref and p.t.ptr == 1 and p.t.canon == "_GUID":
            return i
    return None


def classify_param(types: GuestTypes, m: Method, p: Param, structs: dict[str, Struct]) -> str:
    t = p.t
    h = types.handle_name(t)
    if t.ptr == 0:
        if h == "HWND":
            p.cls = ArgClass.HWND
            return ""
        if h:
            return f"{h} argument"
        if t.tptr:
            return "pointer typedef"
        if t.kind == TypeKind.BUILTIN and t.canon == "float":
            p.cls = ArgClass.FLOAT
            return ""
        if t.kind in (TypeKind.BUILTIN, TypeKind.ENUM):
            p.cls = ArgClass.SCALAR32
            return ""
        return f"{t.kind} by value"
    if t.ptr == 1 and t.base == "HANDLE":
        p.cls = ArgClass.HANDLE_NULL
        return ""
    if t.kind == TypeKind.INTERFACE:
        if t.ptr == 1:
            p.cls = ArgClass.IFACE_IN
            return ""
        if t.ptr == 2:
            p.cls = ArgClass.IFACE_OUT
            return ""
        return "interface pointer depth"
    if t.ptr == 2 and t.kind == TypeKind.VOID and iid_param(m) is not None:
        p.cls = ArgClass.IID_OUT
        return ""
    if t.ptr != 1 or t.tptr:
        return "pointer to a pointer"
    if t.kind == TypeKind.VOID:
        p.cls = ArgClass.DATA_PTR
        return ""
    if t.kind in (TypeKind.BUILTIN, TypeKind.ENUM):
        p.cls = ArgClass.SCALAR_PTR
        return ""
    if t.kind == TypeKind.RECORD:
        s = structs[t.canon]
        p.struct = t.canon
        if s.verdict == Verdict.PASS:
            p.cls = ArgClass.STRUCT_PASS
            return ""
        if s.verdict == Verdict.CONV:
            p.cls = ArgClass.STRUCT_CONV
            if t.const:
                p.direction, p.note = Direction.IN, "const"
            elif m.name.startswith("Get"):
                p.direction, p.note = Direction.OUT, "non-const, getter (name starts with Get)"
            else:
                p.direction, p.note = Direction.INOUT, "non-const, not a getter: the callee may read and write it"
            return ""
        return f"struct {s.spell}: {s.reason}"
    return f"pointer to {t.kind} {t.canon}"


# ---- emit ------------------------------------------------------------------

def c_ident(path: str) -> str:
    return path.replace(".", "_")


def host_cast(t: TypeInfo) -> str:
    """The C spelling of a pointer parameter for the host call (a C++
    reference such as REFGUID becomes a pointer in C)."""
    base = ("const " if t.const else "") + t.base
    return base + " " + "*" * t.ptr


def emit_struct_conv(s: Struct, types: GuestTypes, dirs: set[Direction], out: list[str]) -> None:
    out.append(f"/* {s.spell}: {s.reason}. */")
    if dirs & {Direction.IN, Direction.INOUT}:
        emit_copy(s, types, True, out)
    if dirs & {Direction.OUT, Direction.INOUT}:
        emit_copy(s, types, False, out)
    out.append("")


def emit_copy(s: Struct, types: GuestTypes, copy_in: bool, out: list[str]) -> None:
    g = s.guest
    if not copy_in:
        out.append(f"static void d3d9g_out_{s.key}(uint32_t g, const {s.spell} *h)")
        out.append("{")
        for lf in s.leaves:
            off = g[f"off|{lf.path}"]
            if types.handle_name(lf.t) == "HWND":
                out.append(f"    MEM32(g + {off}u) = d3d9g_hwnd_out(h->{lf.path});")
            else:
                out.append(f"    memcpy(guest_ptr(g + {off}u), &h->{lf.path}, {g[f'lsize|{lf.path}']}u);")
        out.append("}")
        return
    out.append(f"static void d3d9g_in_{s.key}({s.spell} *h, uint32_t g)")
    out.append("{")
    for lf in s.leaves:
        off = g[f"off|{lf.path}"]
        if types.handle_name(lf.t) == "HWND":
            out.append(f"    h->{lf.path} = d3d9g_hwnd_in(MEM32(g + {off}u));")
        else:
            out.append(f"    memcpy(&h->{lf.path}, guest_ptr(g + {off}u), {g[f'lsize|{lf.path}']}u);")
    out.append("}")


def ret_assign(m: Method) -> tuple[str, str]:
    """(declaration prefix for the call, return statement)."""
    n = m.slots
    if m.ret_cls == RetClass.VOID:
        return "", f"    g_esp += 4u + 4u * {n}u;"
    if m.ret_cls == RetClass.FLOAT:
        return "float r = ", f"    d3d9g_fp_push(r);\n    g_esp += 4u + 4u * {n}u;"
    return f"{m.ret} r = ", f"    W32_RETURN({n}, (uint32_t)r);"


def emit_method(m: Method, types: GuestTypes, out: list[str]) -> None:
    n = m.slots
    out.append(f"static void {m.cname}(void)")
    out.append("{")
    if m.kind == Kind.FALLBACK:
        ret = "D3DERR_INVALIDCALL" if m.ret == "HRESULT" else "0"
        out.append("    static int once;")
        out.append("    if (!__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED))")
        out.append(f'        fprintf(stderr, "UNSUPPORTED d3d9: {m.iface}::{m.name}: {c_str(m.reason)}\\n");')
        if m.ret_cls == RetClass.VOID:
            out.append(f"    g_esp += 4u + 4u * {n}u;")
        elif m.ret_cls == RetClass.FLOAT:
            out.append("    d3d9g_fp_push(0.0f);")
            out.append(f"    g_esp += 4u + 4u * {n}u;")
        else:
            out.append(f"    W32_RETURN({n}, {ret});")
        out.append("}")
        out.append("")
        return
    out.append(f"    uint32_t g_this = W32_ARG(0);")
    for i, _p in enumerate(m.params, 1):
        out.append(f"    uint32_t a{i} = W32_ARG({i});")
    out.append(f"    {m.iface} *self = ({m.iface} *)com_guest_self(g_this);")
    pre, post, args = [], [], ["self"]
    if m.kind == Kind.SPECIAL:
        if m.name == "QueryInterface":
            out.append("    void *obj = NULL;")
            out.append("    HRESULT r = self->lpVtbl->QueryInterface(self, (const IID *)d3d9g_gp(a1), a2 ? &obj : NULL);")
            out.append("    if (a2 != 0u)")
            out.append("        MEM32(a2) = d3d9g_wrap_iid(obj, a1, com_guest_iface(g_this), &r);")
        elif m.name == "AddRef":
            out.append("    ULONG r = self->lpVtbl->AddRef(self);")
        else:
            out.append("    ULONG r = self->lpVtbl->Release(self);")
            out.append("    if (r == 0u) {")
            out.append("        d3d9g_forget(self);")
            out.append("        com_guest_free(g_this);")
            out.append("    }")
        out.append(f"    W32_RETURN({n}, (uint32_t)r);")
        out.append("}")
        out.append("")
        return
    for i, p in enumerate(m.params, 1):
        a, t = f"a{i}", p.t
        c = p.cls
        if c == ArgClass.SCALAR32:
            args.append(f"({t.spelling}){a}")
        elif c == ArgClass.FLOAT:
            args.append(f"d3d9g_f32({a})")
        elif c == ArgClass.HWND:
            args.append(f"d3d9g_hwnd_in({a})")
        elif c == ArgClass.HANDLE_NULL:
            pre.append(f"    if ({a} != 0u)")
            pre.append(f'        d3d9g_die("{m.iface}::{m.name}: {p.name} is not NULL");')
            args.append("NULL")
        elif c == ArgClass.IFACE_IN:
            args.append(f"({t.canon} *)d3d9g_self_or_null({a})")
        elif c == ArgClass.IFACE_OUT:
            pre.append(f"    {t.canon} *o{i} = NULL;")
            args.append(f"{a} ? &o{i} : NULL")
            post.append(f"    if ({a} != 0u)")
            post.append(f"        MEM32({a}) = d3d9g_wrap(o{i}, &d3d9g_iface_{t.canon});")
        elif c == ArgClass.IID_OUT:
            if m.ret != "HRESULT":
                die(f"{m.iface}::{m.name}: iid_out needs an HRESULT method")
            pre.append(f"    void *o{i} = NULL;")
            args.append(f"{a} ? &o{i} : NULL")
            post.append(f"    if ({a} != 0u)")
            post.append(f"        MEM32({a}) = d3d9g_wrap_iid(o{i}, a{iid_param(m)}, NULL, &r);")
        elif c in (ArgClass.DATA_PTR, ArgClass.SCALAR_PTR, ArgClass.STRUCT_PASS):
            args.append(f"({host_cast(t)})d3d9g_gp({a})")
        elif c == ArgClass.STRUCT_CONV:
            s = p.struct
            spell = t.base
            pre.append(f"    {spell} h{i};")
            pre.append(f"    {spell} *p{i} = NULL;")
            pre.append(f"    if ({a} != 0u) {{")
            pre.append(f"        memset(&h{i}, 0, sizeof h{i});")
            if p.direction in (Direction.IN, Direction.INOUT):
                pre.append(f"        d3d9g_in_{s}(&h{i}, {a});")
            pre.append(f"        p{i} = &h{i};")
            pre.append("    }")
            args.append(f"p{i}")
            if p.direction in (Direction.OUT, Direction.INOUT):
                post.append(f"    if (p{i} != NULL)")
                post.append(f"        d3d9g_out_{s}({a}, p{i});")
        else:
            die(f"no emitter for class {c}")
    out += pre
    if (m.iface, m.name) in BEFORE_CALL:
        out.append(f"    {BEFORE_CALL[(m.iface, m.name)]}")
    decl, ret = ret_assign(m)
    out.append(f"    {decl}self->lpVtbl->{m.name}({', '.join(args)});")
    out += post
    if (m.iface, m.name) in AFTER_CALL:
        out.append(f"    {AFTER_CALL[(m.iface, m.name)]}")
    out.append(ret)
    out.append("}")
    out.append("")


def c_str(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def guid_bytes(vals: list[int]) -> list[int]:
    d1, d2, d3, *d4 = vals
    b = list(d1.to_bytes(4, "little")) + list(d2.to_bytes(2, "little")) + list(d3.to_bytes(2, "little"))
    return b + d4


def int_values(node: dict) -> list[int]:
    out: list[int] = []
    if node.get("kind") == "IntegerLiteral":
        out.append(int(node["value"]))
    for x in node.get("inner", []):
        out += int_values(x)
    return out


# DXVK Native defines THIS_ and THIS empty in C as well as C++, which drops
# the This argument from every C vtable entry. Its own IUnknownVtbl and
# d3d9.h's C call macros take This, so restore the MinGW definitions (d3d9.h
# sets INTERFACE before each interface).
HOST_PRELUDE = """#include <windows.h>
#undef THIS_
#undef THIS
#define THIS_ INTERFACE *This,
#define THIS INTERFACE *This
#include <d3d9.h>
"""

GEN_BANNER = "Generated by tools/d3d9/gen_d3d9_guest.py. Do not edit; rerun the generator."


def emit_header(methods: dict[str, list[Method]], structs: dict[str, Struct], extra: list[dict]) -> str:
    o = [f"/* {GEN_BANNER}", " *",
         " * Guest-facing Direct3D 9: one guest COM interface per d3d9.h interface",
         " * (notes/d3d9_marshal.md). The generated thunks live in gen_d3d9_guest.c;",
         " * the helpers and lock methods below them in d3d9_guest_manual.c. */",
         "#ifndef D3D9_GUEST_H", "#define D3D9_GUEST_H", "",
         "#include <stddef.h>", "#include <stdint.h>", "",
         '#include "com_guest.h"', '#include "guest_mem.h"', '#include "recomp_types.h"', "",
         "/* ---- Hooks other slices provide ---- */", "",
         "/* The host window for a guest HWND (UI slice). */",
         "void *d3d9g_hwnd(uint32_t guest_hwnd);", "",
         "/* A new host IDirect3D9 for Direct3DCreate9, or NULL (backend). */",
         "void *d3d9g_backend_create9(uint32_t sdk_version);",
         "/* Size a device window's client area to width x height, or with desktop",
         " * set make it a borderless desktop-sized window, and bring it to the",
         " * front; never a display mode change (backend). */",
         "void d3d9g_backend_fit_window(void *host_window, uint32_t width, uint32_t height, int desktop);",
         "/* Show a device window, as Windows Direct3D 9 does for a fullscreen",
         " * device (backend). */",
         "void d3d9g_backend_show_window(void *host_window);",
         "/* The main display below any notch: the desktop's scaled size in points,",
         " * the panel's own size in pixels, and the refresh rate in Hz (backend). */",
         "void d3d9g_backend_screen_sizes(uint32_t *desk_w, uint32_t *desk_h, uint32_t *panel_w,",
         "                                uint32_t *panel_h, uint32_t *refresh_hz);", "",
         "/* ---- Interfaces ---- */", ""]
    for i in INTERFACES:
        o.append(f"extern const com_interface_t d3d9g_iface_{i}; /* {len(methods[i])} methods */")
    o += ["", "/* d3d9.dll!Direct3DCreate9 (stdcall, 1 argument). */", "void imp_Direct3DCreate9(void);"]
    for e in extra:
        o.append(f"void {e['fn']}(void);")
    o += ["", "/* ---- Helpers (d3d9_guest_manual.c) ---- */", "",
          "/* The guest object for host object host (NULL gives 0): the existing one,",
          " * or a new one of interface iface. */",
          "uint32_t d3d9g_wrap(void *host, const com_interface_t *iface);",
          "/* The existing guest object for host, or 0. */",
          "uint32_t d3d9g_find(void *host);",
          "/* Drop the map entry and lock staging of a host object whose Release",
          " * returned 0. */",
          "void d3d9g_forget(void *host);",
          "/* Guest HWND to host (0 gives NULL) and back; the pairs come from", " * d3d9g_hwnd_in. */",
          "void *d3d9g_hwnd_in(uint32_t guest_hwnd);", "uint32_t d3d9g_hwnd_out(void *host_hwnd);",
          "/* Print `UNSUPPORTED d3d9: <what>` and abort. */",
          "__attribute__((noreturn)) void d3d9g_die(const char *what);",
          "/* After IDirect3D9::CreateDevice: on success without",
          " * D3DCREATE_FPU_PRESERVE, set the calling thread's x87 control word as",
          " * Windows does (single precision, every exception masked). */",
          "void d3d9g_after_create_device(uint32_t hr, uint32_t behavior_flags);",
          "/* Presentation (d3d9_guest_manual.c). Before CreateDevice, Reset and",
          " * CreateAdditionalSwapChain: unless HPRECOMP_FULLSCREEN=1, the host",
          " * D3DPRESENT_PARAMETERS (host_pp) gets Windowed=TRUE and refresh rate 0,",
          " * and the device window becomes a borderless desktop-sized window, or with",
          " * HPRECOMP_WINDOWED=1 a window the size of the back buffer. The",
          " * presentation interval is always ONE (FIFO; notes/packaging.md,",
          " * \"Display options\"). keep holds the game's values, which",
          " * d3d9g_windowed_out puts back in its struct. */",
          "typedef struct {", "    int forced, vsync;", "    uint32_t windowed, refresh_hz, interval;",
          "} d3d9g_mode_keep;",
          "void d3d9g_windowed_in(void *host_pp, d3d9g_mode_keep *keep, void *focus_window);",
          "void d3d9g_windowed_out(uint32_t guest_pp, const d3d9g_mode_keep *keep);",
          "/* After GetAdapterModeCount, EnumAdapterModes and GetAdapterDisplayMode:",
          " * in the borderless desktop default the adapter lists the panel size, 75%,",
          " * the desktop size (the default) and 50%, all below any notch. */",
          "uint32_t d3d9g_mode_count(uint32_t host_count);",
          "uint32_t d3d9g_enum_mode(uint32_t hr, uint32_t format, uint32_t index, uint32_t guest_mode);",
          "void d3d9g_display_mode(uint32_t hr, uint32_t guest_mode);",
          "/* Before IDirect3DDevice9::Present: with HPRECOMP_DUMP_FRAMES=n1,n2,...",
          " * write the back buffer of those Presents (1-based) to",
          " * $HPRECOMP_DUMP_DIR/frame_<n>.ppm through host calls only. */",
          "void d3d9g_dump_frame(void *host_device);",
          "/* HP_TRACE_D3D9 events: a line per Present, EndScene and Reset, one",
          " * when TestCooperativeLevel's result changes, and a per-second summary. */",
          "enum d3d9g_trace_event {", "    D3D9G_TR_PRESENT,", "    D3D9G_TR_END_SCENE,", "    D3D9G_TR_DRAW,",
          "    D3D9G_TR_SET_RENDER_TARGET,", "    D3D9G_TR_RESET,", "    D3D9G_TR_TEST_COOPERATIVE_LEVEL,",
          "    D3D9G_TR_COUNT", "};", "void d3d9g_trace(enum d3d9g_trace_event ev, uint32_t hr);", "",
          "static inline void *d3d9g_gp(uint32_t a)", "{", "    return a != 0u ? guest_ptr(a) : NULL;", "}", "",
          "static inline void *d3d9g_self_or_null(uint32_t a)", "{",
          "    return a != 0u ? com_guest_self(a) : NULL;", "}", "",
          "/* ---- Hand-written methods (d3d9_guest_manual.c) ---- */", ""]
    for i in INTERFACES:
        for m in methods[i]:
            if m.kind == Kind.MANUAL:
                o.append(f"void {m.cname}(void);")
    o += ["", "/* ---- Guest (i686) layouts the hand-written methods use ---- */", ""]
    for name in MANUAL_STRUCTS:
        s = next(s for s in structs.values() if s.spell == name)
        o.append(f"#define D3D9G_GSIZE_{name} {s.guest['size']}u")
        for lf in s.leaves:
            o.append(f"#define D3D9G_GOFF_{name}_{c_ident(lf.path)} {s.guest[f'off|{lf.path}']}u")
    o += ["", "#endif", ""]
    return "\n".join(o)


def emit_c(methods: dict[str, list[Method]], structs: dict[str, Struct], types: GuestTypes,
           iids: dict[str, list[int]], extra: list[dict]) -> str:
    o = [f"/* {GEN_BANNER}", " *",
         " * Host thunks for every Direct3D 9 interface method: guest stdcall",
         " * arguments in, host call through the C vtable, result in g_eax (or st(0)),",
         " * 4 + 4 * slots bytes popped. ebx/esi/edi/ebp are never touched. */",
         "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>", "",
         "/* DXVK Native's THIS_/THIS are empty in C; see HOST_PRELUDE in the",
         " * generator. */", *HOST_PRELUDE.rstrip("\n").split("\n"), "",
         '#include "d3d9_guest.h"', '#include "d3d9_hud.h"', '#include "win32_thunk.h"', "",
         "static inline float d3d9g_f32(uint32_t bits)", "{", "    float f;",
         "    memcpy(&f, &bits, sizeof f);", "    return f;", "}", "",
         "/* An x87 return value is st(0). */",
         "static inline void d3d9g_fp_push(float v)", "{",
         "    g_fp_top = (g_fp_top + 7u) & 7u;", "    g_fp_stack[g_fp_top] = (double)v;", "}", "",
         "/* ---- Struct layouts ---- */", ""]
    for s in sorted(structs.values(), key=lambda s: s.spell):
        if s.verdict == Verdict.PASS and s.users:
            o.append(f'_Static_assert(sizeof({s.spell}) == {s.guest["size"]}, '
                     f'"{s.spell}: host size differs from the guest; rerun gen_d3d9_guest.py");')
    o.append("")
    dirs: dict[str, set[Direction]] = {}
    for i in INTERFACES:
        for m in methods[i]:
            for p in m.params:
                if m.kind == Kind.GENERATED and p.cls == ArgClass.STRUCT_CONV and p.direction is not None:
                    dirs.setdefault(p.struct, set()).add(p.direction)
    for s in sorted(structs.values(), key=lambda s: s.spell):
        if s.key in dirs:
            emit_struct_conv(s, types, dirs[s.key], o)
    o += ["/* ---- QueryInterface ---- */", "",
          "static const struct {", "    uint8_t iid[16];", "    const com_interface_t *iface;",
          "} d3d9g_iids[] = {"]
    for i in INTERFACES:
        b = guid_bytes(iids[f"IID_{i}"])
        o.append("    {{" + ", ".join(f"0x{x:02X}" for x in b) + f"}}, &d3d9g_iface_{i}}},")
    o.append("};")
    b = guid_bytes(iids["IID_IUnknown"])
    o += ["static const uint8_t d3d9g_iid_unknown[16] = {" + ", ".join(f"0x{x:02X}" for x in b) + "};", "",
          "/* The guest object for an object returned by IID (QueryInterface,",
          " * GetContainer). IUnknown gives the existing guest object, else one of",
          " * interface unknown_iface; an IID outside d3d9.h is refused. */",
          "static uint32_t d3d9g_wrap_iid(void *obj, uint32_t riid, const com_interface_t *unknown_iface,",
          "                               HRESULT *hr)",
          "{", "    if (obj == NULL)", "        return 0;",
          "    const uint8_t *want = guest_ptr(riid);",
          "    for (size_t k = 0; k < sizeof d3d9g_iids / sizeof d3d9g_iids[0]; k++)",
          "        if (memcmp(want, d3d9g_iids[k].iid, 16) == 0)",
          "            return d3d9g_wrap(obj, d3d9g_iids[k].iface);",
          "    if (memcmp(want, d3d9g_iid_unknown, 16) == 0) {",
          "        uint32_t g = d3d9g_find(obj);",
          "        if (g != 0u || unknown_iface != NULL)",
          "            return g != 0u ? g : d3d9g_wrap(obj, unknown_iface);",
          "    }",
          "    ((IUnknown *)obj)->lpVtbl->Release((IUnknown *)obj);",
          '    fprintf(stderr, "UNSUPPORTED d3d9: QueryInterface: IID outside d3d9.h\\n");',
          "    *hr = (HRESULT)COM_E_NOINTERFACE;", "    return 0;", "}", ""]
    for i in INTERFACES:
        o.append(f"/* ---- {i} ---- */")
        o.append("")
        for m in methods[i]:
            if m.kind != Kind.MANUAL:
                emit_method(m, types, o)
        o.append(f"static const recomp_func_t d3d9g_methods_{i}[{len(methods[i])}] = {{")
        for m in methods[i]:
            o.append(f"    {m.cname}, /* {m.index} */")
        o.append("};")
        o.append(f'const com_interface_t d3d9g_iface_{i} = {{"{i}", d3d9g_methods_{i}, {len(methods[i])}u}};')
        o.append("")
    o += ["/* ---- d3d9.dll ---- */", "",
          "void imp_Direct3DCreate9(void)", "{",
          "    void *d3d = d3d9g_backend_create9(W32_ARG(0));",
          "    W32_RETURN(1, d3d9g_wrap(d3d, &d3d9g_iface_IDirect3D9));", "}", "",
          "/* The d3d slice's table. More entries (D3DX) come from the generator's",
          " * --extra-exports input, never from editing this file. */",
          "const win32_export_t win32_exports_d3d[] = {",
          '    {"d3d9.dll", "Direct3DCreate9", 0, imp_Direct3DCreate9},']
    for e in extra:
        name = f'"{e["name"]}"' if e.get("name") else "NULL"
        o.append(f'    {{"{e["dll"]}", {name}, {int(e.get("ordinal", 0))}, {e["fn"]}}},')
    o += ["    {0},", "};", ""]
    return "\n".join(o)


def emit_report(methods: dict[str, list[Method]], structs: dict[str, Struct], types: GuestTypes,
                inputs: list[str], extra: list[dict]) -> str:
    allm = [m for i in INTERFACES for m in methods[i]]
    o = ["# Direct3D 9 guest marshalling", "", f"{GEN_BANNER}", "", "## Inputs", ""]
    o += [f"- {x}" for x in inputs]
    o += ["", "The generated file is C (`runtime/src/gen_d3d9_guest.c`): DXVK Native's",
          "headers compile as C with `lpVtbl` C vtables, and the guest contracts",
          "(`com_guest.h`, `win32_thunk.h`) are C.", "",
          "## Checks the generator makes", "",
          "- Method order: base interface first, then the interface's own methods (C++ AST);",
          "  each index equals the i686 C vtable slot (`offsetof(IxxxVtbl, m) / 4`) and the",
          "  arm64 host C vtable slot (`offsetof(IxxxVtbl, m) / sizeof(void *)`).",
          "- Pop count: 4 + the argument bytes, checked against the `@N` byte count in",
          "  each method's i686 stdcall mangled name.",
          "- Struct layouts: size, alignment and every field's offset and size, probed on",
          "  i686 (MinGW) and arm64 (DXVK).", "",
          "## Argument classes", "", "| Class | Rule | Arguments |", "|---|---|---|"]
    counts: dict[str, int] = {c: 0 for c in ARG_CLASSES}
    for m in allm:
        if m.kind in (Kind.GENERATED, Kind.SPECIAL):
            for p in m.params:
                if p.cls is not None:
                    counts[p.cls] += 1
    for c, rule in ARG_CLASSES.items():
        o.append(f"| {c} | {rule} | {counts[c]} |")
    o += ["", "Return values: 32-bit results in g_eax; `float` (GetNPatchMode) on the x87",
          "stack; `void` leaves g_eax alone. A fallback returns D3DERR_INVALIDCALL, or 0",
          "when the method does not return an HRESULT.", "",
          "## Structs", "",
          "| Struct | Guest size | Host size | Verdict | Why | Used by |", "|---|---|---|---|---|---|"]
    for s in sorted(structs.values(), key=lambda s: s.spell):
        if not s.users:
            continue
        verdict = {Verdict.PASS: "pass-through", Verdict.CONV: "converted", Verdict.UNUSABLE: "not convertible"}[s.verdict]
        users = ", ".join(u.split(":", 1)[1] for u in s.users)
        o.append(f"| {s.spell} | {s.guest['size']} | {s.host.get('size')} | {verdict} | {s.reason} | {users} |")
    named = ["RECT", "D3DRECT", "D3DVIEWPORT9", "D3DMATRIX", "D3DLIGHT9", "D3DMATERIAL9", "D3DCAPS9",
             "D3DSURFACE_DESC", "D3DDISPLAYMODE", "D3DADAPTER_IDENTIFIER9", "D3DVERTEXELEMENT9",
             "D3DPRESENT_PARAMETERS", "D3DGAMMARAMP", "D3DCLIPSTATUS9", "D3DRASTER_STATUS",
             "D3DLOCKED_RECT", "D3DLOCKED_BOX"]
    spells = {s.spell for s in structs.values() if s.users}
    missing = [n for n in named if n not in spells]
    if missing:
        o.append("")
        o.append(f"Named in the design but taken by no method: {', '.join(missing)}.")
    o += ["", "D3DLOCKED_RECT and D3DLOCKED_BOX are used only by the hand-written lock",
          "methods, which write their guest layout (`D3D9G_GOFF_*` in d3d9_guest.h).", "",
          "## Direction of converted struct arguments", "",
          "| Method | Argument | Struct | Direction | Why |", "|---|---|---|---|---|"]
    for m in allm:
        for p in m.params:
            if p.cls == ArgClass.STRUCT_CONV and m.kind == Kind.GENERATED:
                o.append(f"| {m.iface}::{m.name} | {p.name} | {p.t.base} | {p.direction} | {p.note} |")
    o += ["", "An in/out D3DPRESENT_PARAMETERS is one element. A multi-head device",
          "(D3DCREATE_ADAPTERGROUP_DEVICE) passes an array and is not handled.", "",
          "## Methods", ""]
    for i in INTERFACES:
        o += [f"### {i} ({len(methods[i])} methods)", "", "| Slot | Method | Pops | Kind | Arguments |",
              "|---|---|---|---|---|"]
        for m in methods[i]:
            args = ", ".join(f"{p.name}:{p.cls or '-'}" for p in m.params) or "-"
            o.append(f"| {m.index} | {m.name} | {4 + 4 * m.slots} | {m.kind} | {args} |")
        o.append("")
    o += ["## Hand-written methods", "",
          "`runtime/src/d3d9_guest_manual.c`: the lock methods stage the locked bytes in",
          "guest memory (copy in on Lock unless D3DLOCK_DISCARD, copy back on Unlock",
          "unless D3DLOCK_READONLY); GetDC/ReleaseDC print an UNSUPPORTED line and return",
          "D3DERR_INVALIDCALL.", ""]
    o += [f"- {m.iface}::{m.name}" for m in allm if m.kind == Kind.MANUAL]
    o += ["", "## Exports", "",
          "`win32_exports_d3d[]` is defined in gen_d3d9_guest.c with Direct3DCreate9"
          + (f" and {len(extra)} extra entries" if extra else "") + ".",
          "The D3DX entries must be added by rerunning the generator with",
          "`--extra-exports <file.json>` (a list of `{dll, name, ordinal, fn}`), not by",
          "editing the generated file.", "",
          "## Host header fix", "",
          "DXVK Native defines `THIS_` and `THIS` empty in C as well as C++, so its",
          "C vtables lose the `This` argument, while its own IUnknownVtbl and d3d9.h's",
          "C call macros take it. Every host translation unit (generated file, manual",
          "file, test) restores the MinGW definitions before `<d3d9.h>` (HOST_PRELUDE in",
          "the generator). This changes no slot: the generator checks every host slot",
          "index against the guest's.", "",
          "## Limits", "",
          "- Object identity: a host object returned again comes back as its first",
          "  guest object, even when the declared type is wider (for example a texture",
          "  first seen as IDirect3DBaseTexture9). That case prints an UNSUPPORTED line.",
          "- A host object destroyed while the guest still holds references (the",
          "  guest leaked them) leaves a stale map entry for that host address.",
          "- Out-direction HWNDs (GetCreationParameters, GetPresentParameters) map back",
          "  through the pairs d3d9g_hwnd_in recorded; an unknown host window gives 0",
          "  and an UNSUPPORTED line.",
          "- Lock staging copies in even for D3DUSAGE_WRITEONLY resources (only",
          "  D3DLOCK_DISCARD skips the copy): the copy back covers the whole locked",
          "  range, so bytes the guest did not write must hold the host's contents.",
          "  With a sub-rectangle and D3DLOCK_DISCARD, the bytes between rows outside",
          "  the rectangle are undefined, as D3D9 defines them after a discard.",
          "- A rectangle or box lock needs the format's texel size: a table of the",
          "  common formats in d3d9_guest_manual.c; others print UNSUPPORTED.", "",
          "## Method counts", "", "| Kind | Methods |", "|---|---|"]
    kinds = [Kind.GENERATED, Kind.SPECIAL, Kind.MANUAL, Kind.FALLBACK]
    for k in kinds:
        o.append(f"| {k} | {sum(1 for m in allm if m.kind == k)} |")
    o.append(f"| total ({len(INTERFACES)} interfaces) | {len(allm)} |")
    fb = [m for m in allm if m.kind == Kind.FALLBACK]
    o += ["", f"## Fallbacks (UNSUPPORTED, {len(fb)})", ""]
    o += [f"- {m.iface}::{m.name}: {m.reason}" for m in fb] or ["None."]
    o.append("")
    return "\n".join(o)


def main() -> None:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("--dxvk", required=True, help="DXVK checkout (include/native)")
    ap.add_argument("--mingw-include", default=DEFAULT_MINGW)
    ap.add_argument("--extra-exports", type=Path)
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--out-c", type=Path, default=REPO / "runtime/src/gen_d3d9_guest.c")
    ap.add_argument("--out-h", type=Path, default=REPO / "runtime/include/d3d9_guest.h")
    ap.add_argument("--report", type=Path, default=REPO / "notes/d3d9_marshal.md")
    a = ap.parse_args()

    res = subprocess.run([a.clang, "-print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip()
    dx = Path(a.dxvk) / "include/native"
    guest = Target("i686 guest", ["-target", "i686-w64-windows-gnu", "-nostdinc",
                                  "-isystem", f"{res}/include", "-isystem", a.mingw_include])
    host = Target("arm64 host", ["-arch", "arm64", "-I", str(dx / "directx"), "-I", str(dx / "windows")])
    prelude_g = "#define WIN32_LEAN_AND_MEAN\n#include <windows.h>\n#include <d3d9.h>\n"
    extra = json.loads(a.extra_exports.read_text()) if a.extra_exports else []

    types = GuestTypes(ast_dump(a.clang, guest, prelude_g, "c++"))
    methods = build_methods(types)

    iid_nodes = ast_dump(a.clang, guest, "#define INITGUID\n" + prelude_g, "c", "IID_I")
    iids: dict[str, list[int]] = {}
    for n in iid_nodes:
        if n.get("kind") == "VarDecl" and n.get("inner"):
            v = int_values(n)
            if len(v) == 11:
                iids[n["name"]] = v
    for name in ["IID_IUnknown"] + [f"IID_{i}" for i in INTERFACES]:
        if name not in iids:
            die(f"{name} has no value in the guest headers")

    # Structs reached through method pointers, plus the lock structs.
    structs: dict[str, Struct] = {}

    def add_struct(t: TypeInfo, user: str) -> None:
        if t.kind != TypeKind.RECORD or t.tptr or t.anon:
            return
        s = structs.get(t.canon)
        if s is None:
            s = structs[t.canon] = Struct(t.canon, t.base, flatten(types, types.records[t.canon], ""))
        if user and user not in s.users:
            s.users.append(user)

    for i in INTERFACES:
        for m in methods[i]:
            for p in m.params:
                if p.t.ptr == 1:
                    add_struct(p.t, ("man:" if (i, m.name) in MANUAL else "gen:") + f"{i}::{m.name}")
    for name in MANUAL_STRUCTS:
        add_struct(types.resolve(name), "")

    # Probes.
    sizes = sorted({p.t.spelling for i in INTERFACES for m in methods[i] for p in m.params if p.t.ptr == 0}
                   | {m.ret for i in INTERFACES for m in methods[i] if m.ret != "void"})
    g_items = [(f"psize|{s}", f"sizeof({s})") for s in sizes]
    h_items: list[tuple[str, str]] = []
    for s in structs.values():
        g_items += struct_items(s)
        h_items += struct_items(s)
    for i in INTERFACES:
        for m in methods[i]:
            g_items.append((f"vt|{i}|{m.name}", f"offsetof({i}Vtbl, {m.name}) / sizeof(void *)"))
            h_items.append((f"vt|{i}|{m.name}", f"offsetof({i}Vtbl, {m.name}) / sizeof(void *)"))
    gv = probe(a.clang, guest, prelude_g, g_items)
    hv = probe(a.clang, host, HOST_PRELUDE, h_items)
    for key, v in gv.items():
        if v is None:
            die(f"guest probe failed: {key}")
    for i in INTERFACES:
        for m in methods[i]:
            if gv[f"vt|{i}|{m.name}"] != m.index:
                die(f"{i}::{m.name}: AST order {m.index}, i686 C vtable slot {gv[f'vt|{i}|{m.name}']}")
            if hv[f"vt|{i}|{m.name}"] != m.index:
                die(f"{i}::{m.name}: guest slot {m.index}, host C vtable slot {hv[f'vt|{i}|{m.name}']}")
    for s in structs.values():
        pre = f"{s.key}|"
        s.guest = {k[len(pre):]: v for k, v in gv.items() if k.startswith(pre)}
        s.host = {k[len(pre):]: v for k, v in hv.items() if k.startswith(pre)}
        judge(types, s)
    guest_sizes = {k.split("|", 1)[1]: v for k, v in gv.items() if k.startswith("psize|")}
    classify(types, methods, structs, guest_sizes)

    rev = subprocess.run(["git", "-C", str(a.dxvk), "rev-parse", "HEAD"], capture_output=True, text=True,
                         check=False).stdout.strip() or "unknown"
    rev_dx = subprocess.run(["git", "-C", str(dx / "directx"), "rev-parse", "HEAD"], capture_output=True,
                            text=True, check=False).stdout.strip() or "unknown"
    inputs = [f"Guest: `{a.mingw_include}/d3d9.h` (i686-w64-windows-gnu)",
              f"Host: DXVK `{rev}`, include/native/directx `{rev_dx}` (arm64-apple-macos)"]
    a.out_h.write_text(emit_header(methods, structs, extra))
    a.out_c.write_text(emit_c(methods, structs, types, iids, extra))
    a.report.write_text(emit_report(methods, structs, types, inputs, extra))
    allm = [m for i in INTERFACES for m in methods[i]]
    print(f"gen_d3d9_guest: {len(INTERFACES)} interfaces, {len(allm)} methods: "
          + ", ".join(f"{k} {sum(1 for m in allm if m.kind == k)}"
                      for k in (Kind.GENERATED, Kind.SPECIAL, Kind.MANUAL, Kind.FALLBACK)))


if __name__ == "__main__":
    main()
