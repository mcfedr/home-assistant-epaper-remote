from pathlib import Path

Import("env")


def patch_i2c_probe(*_args, **_kwargs):
    libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
    pioenv = env.subst("$PIOENV")
    target = libdeps / pioenv / "bb_captouch" / "src" / "bb_captouch.cpp"

    if not target.exists():
        print(f"[patch_bb_captouch] skip: {target} not found")
        return False

    source = target.read_text()
    old = (
        "  myWire->beginTransmission(u8Addr);\n"
        "  return(myWire->endTransmission(true) == 0);"
    )
    new = (
        "  // Zero-length write probes false-positive on arduino-esp32 3.x (IDF5 I2C\n"
        "  // driver), misdetecting the GT911 bus as MXT144. Require a readable byte.\n"
        "  if (myWire->requestFrom(u8Addr, (uint8_t)1) != 1) return false;\n"
        "  myWire->read();\n"
        "  return true;"
    )

    if new in source:
        print("[patch_bb_captouch] already applied")
        return True

    if old not in source:
        print("[patch_bb_captouch] warning: expected I2CTest body not found")
        return False

    target.write_text(source.replace(old, new, 1))
    print("[patch_bb_captouch] patched I2CTest probe in bb_captouch")
    return True


patched_now = patch_i2c_probe()

if not patched_now:
    env.AddPreAction("buildprog", patch_i2c_probe)
