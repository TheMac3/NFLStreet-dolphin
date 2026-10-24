"""Pickup draft policies for GN7E69 revision 0 / Reborn 2026.09.12.

The patch replaces only turn advancement after a valid, non-final pick.
Native pool checks, roster writes, completion and other modes remain native.
Runtime choices join the verified injection bundle and room fingerprint.
The image recipes remain a reproducible compatibility and oracle path. See docs/draftmodes.md for evidence and limitations.
"""
from __future__ import annotations

import isopatch as ip

ORDERS = ("alternating", "snake", "cocky")
FIRSTS = ("stock", "team-one", "team-two")
ADVANCE_SITE = 0x8001ABAC
ADVANCE_RETURN = 0x8001AC34
INIT_SITE = 0x80018FC0
FIRST_TEAM_FN = 0x8007CB6C
CODE = 0x80411600  # dormant profile cave; rollback and profile are mutually exclusive
INIT_CODE = CODE + 0x100
SELECT_READ_SITE = 0x800114F4
SELECT_COMMIT_SITE = 0x80010B34
SELECT_FIRST = 0x803EB96A
SELECT_READ_CODE = INIT_CODE + 0x40
SELECT_COMMIT_CODE = INIT_CODE + 0x80
TABLE = 0x803EBA08
MODE = 0x803EB2AC
PICKUP = 15


def patch_names(order: str, first: str) -> set[str]:
    """Encode validated setup choices; stock defaults preserve the release image."""
    if order not in ORDERS or first not in FIRSTS:
        raise ip.PatchError("invalid Pickup draft order or first team")
    return ({f"draft-{order}"} if order != "alternating" else set()) | (
        {f"draft-first-{first}"} if first != "stock" else set())


def validate_patches(patches: set[str]) -> None:
    """Refuse conflicting choices and keep this recipe on its tested rollback base."""
    choices = patches & {"draft-snake", "draft-cocky"}
    firsts = patches & {"draft-first-team-one", "draft-first-team-two"}
    if len(choices) > 1 or len(firsts) > 1:
        raise ip.PatchError("choose one draft order and one first team")
    if (choices or firsts) and "rollback" not in patches:
        raise ip.PatchError("draft choices need rollback")


def _assemble(address: int, program: list) -> list[int]:
    """Resolve local labels for the two small, leaf PowerPC trampolines."""
    labels, pc = {}, address
    for item in program:
        if isinstance(item, str):
            labels[item] = pc
        else:
            pc += 4
    result = []
    for item in program:
        if isinstance(item, str):
            continue
        pc = address + len(result) * 4
        if isinstance(item, tuple):
            op, target = item
            target = labels.get(target, target)
            result.append(ip.b(pc, target) if op == "b" else
                          ip.bc(pc, target, 12 if op in ("beq", "blt") else 4,
                                0 if op == "blt" else 2))
        else:
            result.append(item)
    return result


def advance_routine(order: str) -> list[int]:
    """Choose the next drafter from committed counts, with no persistent counter.

    The initial slot is always zero but may represent either team. Preserve
    that mapping using old_team XOR old_slot XOR next_slot. The native path
    owns the final pick, so neither seven-player list can overflow. r3 stays
    zero (native completion result); only volatile scratch registers/CR change.
    """
    prog = [ip.lwz(11, -0x6898, 13), ip.lwz(9, -0x6FF4, 13),
            ip.cmpwi(9, PICKUP), ("bne", "native"),
            ip.lbz(9, 0x14, 11), ip.cmpwi(9, 2), ("bne", "native"),
            ip.lbz(9, 0x16, 11), ip.cmpwi(9, 7), ("bne", "native"),
            ip.lwz(9, 4, 11), ip.lbz(10, 0, 9), ip.lbz(9, 1, 9), ip.add(10, 10, 9)]
    if order == "snake":
        # n completed picks -> ((n + 1) >> 1) & 1: B,B,A,A,...
        prog += [ip.addi(10, 10, 1), ip.rlwinm(10, 10, 31, 31, 31)]
    elif order == "cocky":
        prog += [ip.cmpwi(10, 7), ip.addi(10, 0, 0), ("blt", "chosen"),
                 ip.addi(10, 0, 1), "chosen"]
    else:
        raise ip.PatchError("advance hook needs snake or cocky")
    # xor r9,r9,r0; xor r9,r9,r10 (no extra encoder in the common module).
    prog += [ip.lbz(9, 0x19, 11), ip.lbz(0, 0x15, 11),
             0x7D290278, 0x7D295278, ip.stb(10, 0x15, 11), ip.stb(9, 0x19, 11),
             ("b", ADVANCE_RETURN), "native", ip.lwz(11, -0x6898, 13),
             ("b", ADVANCE_SITE + 4)]
    words = _assemble(CODE, prog)
    assert CODE + len(words) * 4 <= INIT_CODE
    return words


def init_routine(first: str) -> list[int]:
    """Set which team owns slot zero; retain the stock query outside Pickup.

    Native init negates this result and builds both controller mappings from
    it. Choosing the opposite GTPG here changes both mappings consistently.
    Team One/Two are native game sides, not host identity or controller ports.
    """
    if first not in FIRSTS[1:]:
        raise ip.PatchError("first-team hook needs an explicit team")
    words = _assemble(INIT_CODE, [ip.lwz(9, -0x6FF4, 13), ip.cmpwi(9, PICKUP),
                                  ("bne", "native"),
                                  ip.addi(3, 0, 1 if first == "team-one" else 0), ip.blr(),
                                  "native", ("b", FIRST_TEAM_FN)])
    assert INIT_CODE + len(words) * 4 <= CODE + 0x180
    return words


def select_routine(first: str, *, commit: bool = False) -> list[int]:
    """Keep Select Controller's displayed and committed GTPG choice coherent.

    GN7E69 r0: the screen getter reads r13-0x6936, and its accept path
    passes that byte to the native GTPG setter. Clamp both for an explicit
    room choice; Stock installs neither hook. The native setter still owns
    the database update, so ball-first and draft-first use the same value.
    """
    if first not in FIRSTS[1:]:
        raise ip.PatchError("Select Controller hook needs an explicit team")
    code = SELECT_COMMIT_CODE if commit else SELECT_READ_CODE
    site = SELECT_COMMIT_SITE if commit else SELECT_READ_SITE
    register = 4 if commit else 0
    return _assemble(code, [ip.lwz(9, -0x6FF4, 13), ip.cmpwi(9, PICKUP),
                            ("bne", "native"),
                            ip.addi(register, 0, 1 if first == "team-one" else 0),
                            ip.stb(register, -0x6936, 13), "native",
                            ip.lbz(register, -0x6936, 13), ("b", site + 4)])


def apply(iso, patches: set[str]) -> None:
    """Write guarded original-code recipes into an already copied private image."""
    validate_patches(patches)
    if any(name.startswith("draft-") for name in patches):
        iso.seek(0)
        if iso.read(8) != b"GN7E69\0\0":
            raise ip.PatchError("draft choices require GN7E69 disc 0 revision 0")
    for order in ORDERS[1:]:
        if f"draft-{order}" in patches:
            words = advance_routine(order)
            ip.write_words(iso, CODE, words, expect=[0] * len(words))
            ip.write_words(iso, ADVANCE_SITE, [ip.b(ADVANCE_SITE, CODE)],
                           expect=[ip.lwz(11, -0x6898, 13)])
    for first in FIRSTS[1:]:
        if f"draft-first-{first}" in patches:
            words = init_routine(first)
            ip.write_words(iso, INIT_CODE, words, expect=[0] * len(words))
            ip.write_words(iso, INIT_SITE, [ip.b(INIT_SITE, INIT_CODE, link=True)],
                           expect=[ip.b(INIT_SITE, FIRST_TEAM_FN, link=True)])
            for commit, code, site, register in ((False, SELECT_READ_CODE, SELECT_READ_SITE, 0),
                                                  (True, SELECT_COMMIT_CODE, SELECT_COMMIT_SITE, 4)):
                words = select_routine(first, commit=commit)
                ip.write_words(iso, code, words, expect=[0] * len(words))
                ip.write_words(iso, site, [ip.b(site, code)],
                               expect=[ip.lbz(register, -0x6936, 13)])


def runtime_bytes(content=None, order="alternating", first="stock"):
    """Canonical room settings plus verified ratings; no executable bytes cross the wire.

    The v2 schema deliberately requires a fork that understands the authored
    draft policy. Legacy ratings-only bundles retain their original identity.
    """
    import json
    patch_names(order, first)
    if (order, first) == ("alternating", "stock"):
        return content.runtime_bytes() if content else None
    obj = json.loads(content.runtime_bytes()) if content else {"version": 0, "packs": []}
    obj.update(format="nsrb.runtime-content.v3" if obj.get("format") == "nsrb.runtime-content.v3" else "nsrb.runtime-content.v2", draft={"order": order, "first": first, "policy": 1})
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def room_fingerprint(image_fingerprint, order, first):
    """Bind room admission to the image and draft settings before Dolphin boots."""
    import hashlib
    patch_names(order, first)
    if not image_fingerprint or (order, first) == ("alternating", "stock"):
        return image_fingerprint
    digest = hashlib.sha256((image_fingerprint + ':' + order + ':' + first + ':draft-v1').encode()).hexdigest()
    return ip_fingerprint(digest)


def ip_fingerprint(digest):
    """Use the launcher's existing compact fingerprint presentation."""
    return '-'.join(digest[i:i+4].upper() for i in (0,4,8))
