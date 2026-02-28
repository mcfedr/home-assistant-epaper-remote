from pathlib import Path

Import("env")


def patch_glyph_width_guard(*_args, **_kwargs):
    libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    pioenv = env.subst("$PIOENV")
    target = libdeps / pioenv / "FastEPD" / "src" / "bb_ep_gfx.inl"

    if not target.exists():
        print(f"[patch_fastepd] skip: {target} not found")
        return False

    source = target.read_text()
    old = "if (char_width > 1) { // skip this if drawing a space"
    new = "if (char_width > 0) { // draw all non-empty glyphs (e.g. lowercase l can be 1px wide)"

    if new in source:
        print("[patch_fastepd] already applied")
        return True

    if old not in source:
        print("[patch_fastepd] warning: expected glyph-width guard not found")
        return False

    target.write_text(source.replace(old, new, 1))
    print("[patch_fastepd] patched glyph-width guard in FastEPD")
    return True


# Patch as soon as the script is loaded so the library compiles with the fix.
patched_now = patch_glyph_width_guard()

# Keep a pre-action fallback in case dependency install timing differs.
if not patched_now:
    env.AddPreAction("buildprog", patch_glyph_width_guard)
