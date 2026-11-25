#!/usr/bin/env python3
"""Insert small guards into the lifted C, at fixed guest addresses.

An override (src/ssx3_overrides.c) replaces a whole function. Some fixes need
one test in the middle of a long one instead: a few lines here, re-applied
after every lift, rather than a hand copy of hundreds of generated lines that
would go stale.

Each guard names the label it follows ("loc_XXXXXXXX: ;") and the C it puts
right after it. Every occurrence is patched (a function the lifter also
emitted as a fragment has the label twice). Runs in place over
src/recomp/gen/*.c (scripts/regen.sh calls it after the lift); idempotent:
a guard already present is left alone. Exits non-zero when a guard's label is
missing, so a lift that moved the code is noticed instead of silently losing
the fix.
"""
import sys
from pathlib import Path

# The frontend list widget's input handler, sub_00205770 (thiscall, esi = the
# list, ret 4). With the A button on an item it tells the list's owner
# (this+0x5C, its handler object at +0xD0) "item activated" (vtable +0x48)
# and then "selection changed" (+0x4C). On the Rewards screen (0028D2D0:46)
# the first of those tears the list down and its memory is cleared, so the
# code after it read the owner as 0 and called through words at guest
# address 0xD0 (0x00080016, then NULL), and the game stalled re-opening a
# file named "\r" built from the cleared state. The console would fault on
# page 0 there; the guard leaves through the function's own "handled" exit
# (loc_00205BAB: pops ebp/ebx/edi/esi, al = 1) when the owner is gone. At
# both labels the frame holds exactly those four saved registers.
#
# The handler object can also be found with its own memory cleared while
# the owner still points at it (vtable 0: the call went to whatever word sits
# at guest address 0x48, 0x00080016), so its vtable is checked too.
_LIST_GUARD = (
    "    { extern int ssx3_ListOwnerOk(uint32_t list, uint32_t slot);"
    " if (!ssx3_ListOwnerOk(esi, 0x%X)) {"
    " extern void ssx3_GuardHit(uint32_t site, uint32_t obj);"
    " ssx3_GuardHit(0x%08Xu, esi); goto loc_00205BAB; } }"
    " /* gen_guards: list torn down */\n"
)

# Leaving Rewards the screen and the frontend objects around it were found
# cleared (see ssx3_GuardHit's surface report), and the callers then called
# through their zero vtables: wild targets (low memory read as a vtable)
# that the runtime cannot tell from code. Each guard skips just that call.
#
# The test is ssx3_CallOk(obj, slot) (ssx3_overrides.c): obj a mapped guest
# address, its vtable inside the game image and vtable[slot] code -- not
# just "non-zero": a freed object's words read back as wild values too
# (0xFE000008, 0x83FFFA7D), and a call through those spun for ever.
def _skip(site, obj_expr, slot, obj, to):
    return (
        "    { extern int ssx3_CallOk(uint32_t obj, uint32_t slot);"
        " if (!ssx3_CallOk(%s, 0x%X)) { extern void ssx3_GuardHit(uint32_t site, uint32_t obj);"
        " ssx3_GuardHit(0x%08Xu, %s); goto %s; } } /* gen_guards: cleared object */\n"
        % (obj_expr, slot, site, obj, to)
    )


# The stack at 0x2059E7. The call at 0x2059E4 ("item activated") is the
# game's own handler -- leaving Rewards it tears the screen down, and calls
# skipped inside it through the dead objects (a skipped stdcall leaves its
# arguments behind) came back with esp off: the guard's exit then popped
# the wrong saved registers and returned the caller garbage (a stack address
# as the frontend object, a filename "\r"), which is what hung the game.
# esp at 0x2059D4 is what a balanced callee leaves at 0x2059E7, so it is
# recorded there and put back there (logged when it differed).
#
# The registers too: the handler's own epilogue popped its saved esi / ebx /
# edi from the shifted stack, so after only esp was put back esi was some
# other object -- the check below then found "the list" cleared and skipped
# the "selection changed" call, the one that completes Back, and Rewards
# stayed open. A balanced callee preserves esi, ebx, edi and esp, so all
# four are put back as they were before the call (edi = eax there: the
# code right after the label does edi = eax).
_ESP_SAVE = (
    "    { extern void ssx3_ListRegsSave(uint32_t sp, uint32_t si, uint32_t bx, uint32_t di);"
    " ssx3_ListRegsSave(esp, esi, ebx, eax); } /* gen_guards: regs saved */\n"
)
_ESP_FIX = (
    "    { extern int ssx3_ListRegsFix(uint32_t sp, uint32_t si, uint32_t bx, uint32_t di);"
    " extern uint32_t ssx3_ListSaved(int k);"
    " if (ssx3_ListRegsFix(esp, esi, ebx, edi)) { esp = ssx3_ListSaved(0); esi = ssx3_ListSaved(1);"
    " ebx = ssx3_ListSaved(2); edi = ssx3_ListSaved(3); } } /* gen_guards: regs restored */\n"
)

GUARDS = [
    ("loc_002059D4: ;", _ESP_SAVE + _LIST_GUARD % (0x48, 0x002059D4)),
    ("loc_002059E7: ;", _ESP_FIX + _LIST_GUARD % (0x4C, 0x002059E7)),
    # (0x203B0A and 0x80982 had guards too: they only ever fired on the garbage
    # registers the unbalanced handler left, and once that was fixed they
    # skipped live widgets -- whole menus went missing. Removed.)
    # sub_000823xx (a screen's teardown): deleting members through vtable [0]
    ("loc_00082415: ;", _skip(0x00082415, "ecx", 0, "ecx", "loc_0008241B")),
    ("loc_00082425: ;", _skip(0x00082425, "ecx", 0, "ecx", "loc_0008242B")),
    ("loc_00082435: ;", _skip(0x00082435, "ecx", 0, "ecx", "loc_0008243B")),
    # sub_001AD6F0 (the app's per-tick callback object): its function +4
    # and its vtable +0x34 (0x83FFFA7D was called from here)
    ("loc_001AD6FA: ;",
     "    { extern int ssx3_CodeOk(uint32_t va); if (!ssx3_CodeOk(eax)) {"
     " extern void ssx3_GuardHit(uint32_t site, uint32_t obj); ssx3_GuardHit(0x001AD6FAu, esi);"
     " goto loc_001AD712; } } /* gen_guards: cleared object */\n"),
    ("loc_001AD70B: ;", _skip(0x001AD70B, "esi", 0x34, "esi", "loc_001AD712")),
    # sub_00097B30, a frontend screen's item handler (thiscall: item, event;
    # esi = the handler). Event 5 opens a screen: this+0x10 is the screen
    # manager, [manager] its screen table, table vtable +0xC makes the screen
    # for item+0x18, sub_00203ED0(manager+0x18, screen) shows it. Event 6
    # (Back) is the same through vtable +0x10. On Rewards (0028D2D0:46) the
    # handler's manager pointer is stale: the "vtable" read through it is a
    # switch jump table at 0x80150 in the code, +0x10 = case label
    # 0x00080016, the call failed, and Back did nothing (Rewards re-opened).
    # ssx3_ScreenMgr records a manager whose table is good at every open and
    # Back, and hands it out when the handler's own is not; the guard then
    # does the Back with it.
    ("loc_00097B6A: ;",
     "    { extern uint32_t ssx3_ScreenMgr(uint32_t handler, uint32_t item, int event);"
     " (void)ssx3_ScreenMgr(esi, eax, 5); } /* gen_guards: screen manager */\n"),
    ("loc_00097B47: ;",
     "    { extern uint32_t ssx3_ScreenMgr(uint32_t handler, uint32_t item, int event);"
     " uint32_t _mgr = ssx3_ScreenMgr(esi, eax, 6);"
     " if (_mgr) { eax = MEM32(eax + 0x18); ecx = MEM32(_mgr); edx = MEM32(ecx);"
     " { uint32_t _icall_esp = g_esp; PUSH32(esp, eax); PUSH32(esp, esi);"
     " { uint32_t _t = MEM32(edx + 0x10); PUSH32(esp, 0x00097B56u);"
     " RECOMP_ICALL_SAFE_AT(_t, _icall_esp, 0x00097B53u); } }"
     " if (!eax) goto loc_00097B85;"
     " ecx = _mgr + 0x18; PUSH32(esp, eax);"
     " PUSH32(esp, 0x00097B66u); RECOMP_ABI_CALL(0x00203ED0u, sub_00203ED0);"
     " goto loc_00097B66; } } /* gen_guards: screen manager */\n"),
    # sub_0007FB60, the screen table's "make screen N" (vtable 0028988C +0x10;
    # Back from Rewards asks it for screen 5, the lodge rider screen). Its
    # switch table at 0x80110 has 23 cases, indexed through the byte table at
    # 0x8016C; the lifter ended the function at 0x7FFF8 and kept only the 18
    # targets before that, so cases 0x80016, 0x80056, 0x80090, 0x800CF and
    # 0x7FF99 went to the indirect jump -- "[ICALL] Failed to resolve VA
    # 0x00080016" and no screen. Each missing case allocates a screen
    # (sub_001A9BD0) and constructs it: the lifter emitted those tails as
    # fragment functions that end in this function's own pop edi / pop esi /
    # ret 8, so a tail call into them is exact. 0x80016 has no fragment of
    # its own: mov al,[0x368445]; push 0; push 0; jne 0x7FFB7; push 0x289B54;
    # push 0x50 -- then the fragment at 0x8002A (the call) finishes it.
    ("if (_jt == 0x0007FFDAu) goto loc_0007FFDA;",
     "    if (_jt == 0x00080016u) { SET_LO8(eax, MEM8(0x00368445u)); PUSH32(esp, 0); PUSH32(esp, 0);"
     " if (LO8(eax)) goto loc_0007FFB7; PUSH32(esp, 0x00289B54u); PUSH32(esp, 0x50);"
     " { extern void sub_0008002A(void); g_seh_ebp = ebp; sub_0008002A(); return; } }"
     " if (_jt == 0x00080056u) { extern void sub_00080056(void); g_seh_ebp = ebp; sub_00080056(); return; }"
     " if (_jt == 0x00080090u) { extern void sub_00080090(void); g_seh_ebp = ebp; sub_00080090(); return; }"
     " if (_jt == 0x000800CFu) { extern void sub_000800CF(void); g_seh_ebp = ebp; sub_000800CF(); return; }"
     " if (_jt == 0x0007FF99u) goto loc_0007FF99; /* gen_guards: switch cases past 0x7FFF8 */\n"),
    # sub_001FFFC0, the graphics manager's shape upload (vtable slot at
    # 0x2F7F0C): a switch on the shape type byte (0x60..0x7E) through the
    # byte table 0x2000B8 and the jump table 0x200094 (9 targets). 0x200000 is
    # a common constant (2 MB) elsewhere in the code, so the lifter took it as
    # a function start, ended this function there and kept only the three DXT
    # cases before it; the other six went through RECOMP_ITAIL's lookup. They
    # are the lifted fragments, which end in this function's own pop / ret 0x14
    # (as the kept cases' tail jumps to 0x200046 do), so a tail call is exact.
    ("if (_jt == 0x001FFFF9u) goto loc_001FFFF9;",
     "    { extern void sub_00200000(void), sub_00200011(void), sub_00200022(void),"
     " sub_00200033(void), sub_00200041(void), sub_00200046(void);"
     " if (_jt == 0x00200000u) { g_seh_ebp = ebp; sub_00200000(); return; }"
     " if (_jt == 0x00200011u) { g_seh_ebp = ebp; sub_00200011(); return; }"
     " if (_jt == 0x00200022u) { g_seh_ebp = ebp; sub_00200022(); return; }"
     " if (_jt == 0x00200033u) { g_seh_ebp = ebp; sub_00200033(); return; }"
     " if (_jt == 0x00200041u) { g_seh_ebp = ebp; sub_00200041(); return; }"
     " if (_jt == 0x00200046u) { g_seh_ebp = ebp; sub_00200046(); return; } }"
     " /* gen_guards: switch cases past 0x200000 */\n"),
    # sub_00178BD0, the file system's open-by-name (esi = the name, cdecl;
    # 0 = not opened). Leaving Rewards it was asked for "\r", a name read
    # out of cleared memory, again and again ("D:\<CR>" failing, the game
    # stuck). A name that starts with a control character is no file: fail
    # it here without touching the disc, and log who asked (once).
    ("loc_00178BD0: ;",
     "    if (MEM8(esi) < 0x20) { extern int ssx3_BadFileName(uint32_t name);"
     " if (ssx3_BadFileName(esi)) { eax = 0; esp += 4; return; } } /* gen_guards: bad file name */\n"),
    # sub_00180220, XCreateSaveGame's check of an existing save's SaveMeta.xbx
    # (path, name; stdcall; edi = the open file, esi = 0 so far). Its failure
    # label: when the file is empty (a save damaged by the old SD delete
    # fallback), close it, write it again with the name asked for and pass
    # (save_repair.c). Without this such a save was "unusable" for good: it
    # could not be loaded, overwritten or deleted.
    ("loc_00180284: ;",
     "    { extern int ssx3_SaveMetaEmpty(uint32_t path); extern void ssx3_SaveMetaWrite(uint32_t path, uint32_t name);"
     " if (ssx3_SaveMetaEmpty(MEM32(ebp + 8))) { PUSH32(esp, edi); g_ebp = ebp; g_seh_ebp = ebp;"
     " PUSH32(esp, 0x0018028Fu); RECOMP_ABI_CALL(0x001807D6u, sub_001807D6);"
     " ssx3_SaveMetaWrite(MEM32(ebp + 8), MEM32(ebp + 0xC)); goto loc_00180298; } }"
     " /* gen_guards: empty SaveMeta.xbx */\n"),
]


def main(paths):
    found = {label: 0 for label, _ in GUARDS}
    for path in paths:
        p = Path(path)
        text = p.read_text()
        lines = text.splitlines(keepends=True)
        out, changed = [], False
        i = 0
        while i < len(lines):
            line = lines[i]
            out.append(line)
            for label, guard in GUARDS:
                if line.strip() == label:
                    found[label] += 1
                    nxt = lines[i + 1] if i + 1 < len(lines) else ""
                    if "gen_guards:" not in nxt:
                        out.append(guard)
                        changed = True
            i += 1
        if changed:
            p.write_text("".join(out))
    missing = [label for label, n in found.items() if n == 0]
    for label in missing:
        print("gen_guards: label not found: " + label, file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
