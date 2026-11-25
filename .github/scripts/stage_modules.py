#!/usr/bin/env python3
"""Stage Xiaomi SM8550 kernel modules into first_stage, recovery, and vendor_dlkm trees.

Resolves full module dependency closures via depmod against the compiled module set
and verifies that every required module listed by LineageOS device/xiaomi/sm8550-common
plus per-device BoardConfig.mk extras is present.
"""

import argparse
import os
import shutil
import subprocess
import sys

# Aligned with LineageOS 24.0 android_device_xiaomi_<device>/BoardConfig.mk
# (common modules including cs35l41_dlkm.ko come from sm8550-common/modules.load.*)
DEVICE_DLKM_EXTRAS = {
    "socrates": [
        "wl2866d.ko",
        "xiaomi_touch.ko",
        "goodix_core.ko",
        "goodix_fod.ko",
    ],
    "vermeer": [
        "aw882xx_dlkm.ko",
        "wl2866d.ko",
        "wl2868c.ko",
        "xiaomi_touch.ko",
        "goodix_core.ko",
        "goodix_fod.ko",
    ],
    "fuxi": [
        "xiaomi_touch.ko",
        "fts_touch_spi.ko",
        "goodix_fod.ko",
    ],
    "nuwa": [
        "wl2866d.ko",
        "xiaomi_touch.ko",
        "fts_touch_spi.ko",
        "goodix_fod.ko",
    ],
}

DEVICE_RECOVERY_EXTRAS = {
    "socrates": ["goodix_core.ko"],
    "vermeer": ["goodix_core.ko"],
    "fuxi": ["fts_touch_spi.ko"],
    "nuwa": ["fts_touch_spi.ko"],
}


def read_module_list(path: str) -> list[str]:
    result: list[str] = []
    with open(path, encoding="utf-8") as fh:
        for raw_line in fh:
            line = raw_line.strip()
            if line and not line.startswith("#"):
                result.append(os.path.basename(line))
    return result


def collect_compiled_modules(kdir: str) -> dict[str, str]:
    all_kos: dict[str, str] = {}
    for root, _, files in os.walk(kdir):
        for name in files:
            if name.endswith(".ko"):
                all_kos[name] = os.path.join(root, name)
    return all_kos


def build_dep_graph(all_kos: dict[str, str], kdir: str, out_parts: str, kver: str) -> dict[str, list[str]]:
    flat_root = os.path.join(out_parts, "_all_flat")
    flat_kdir = os.path.join(flat_root, "lib", "modules", kver)
    os.makedirs(flat_kdir, exist_ok=True)

    for name, path in all_kos.items():
        shutil.copy2(path, os.path.join(flat_kdir, name))
    for meta in ("modules.order", "modules.builtin", "modules.builtin.modinfo"):
        src_meta = os.path.join(kdir, meta)
        if os.path.exists(src_meta):
            shutil.copy2(src_meta, os.path.join(flat_kdir, meta))

    subprocess.run(["depmod", "-b", flat_root, kver], check=True)

    deps: dict[str, list[str]] = {}
    with open(os.path.join(flat_kdir, "modules.dep"), encoding="utf-8") as fh:
        for raw_line in fh:
            line = raw_line.strip()
            if not line or ":" not in line:
                continue
            mod, dep_str = line.split(":", 1)
            deps[os.path.basename(mod.strip())] = [
                os.path.basename(item.strip()) for item in dep_str.split() if item.strip()
            ]
    return deps


def expand_with_deps(
    seed_list: list[str],
    all_kos: dict[str, str],
    deps: dict[str, list[str]],
) -> tuple[list[str], list[str]]:
    selected: list[str] = []
    seen: set[str] = set()
    missing: set[str] = set()

    def visit(mod: str) -> None:
        if mod in seen:
            return
        if mod not in all_kos:
            missing.add(mod)
            return
        seen.add(mod)
        for dep in deps.get(mod, []):
            visit(dep)
        selected.append(mod)

    for mod in seed_list:
        visit(mod)
    return selected, sorted(missing)


def stage_partition(
    name: str,
    mods: list[str],
    load_order: list[str],
    dep_prefix: str,
    all_kos: dict[str, str],
    kdir: str,
    out_parts: str,
    kver: str,
) -> str:
    part_root = os.path.join(out_parts, name)
    part_kdir = os.path.join(part_root, "lib", "modules", kver)
    os.makedirs(part_kdir, exist_ok=True)

    for mod in mods:
        shutil.copy2(all_kos[mod], os.path.join(part_kdir, mod))
    with open(os.path.join(part_kdir, "modules.order"), "w", encoding="utf-8") as fh:
        for mod in mods:
            fh.write(f"{mod}\n")
    for meta in ("modules.builtin", "modules.builtin.modinfo"):
        src_meta = os.path.join(kdir, meta)
        if os.path.exists(src_meta):
            shutil.copy2(src_meta, os.path.join(part_kdir, meta))

    subprocess.run(["depmod", "-b", part_root, kver], check=True)

    dep_path = os.path.join(part_kdir, "modules.dep")
    rewritten: list[str] = []
    with open(dep_path, encoding="utf-8") as fh:
        for raw_line in fh:
            if ":" not in raw_line:
                continue
            left, right = raw_line.strip().split(":", 1)
            l_out = dep_prefix + os.path.basename(left.strip())
            r_items = [dep_prefix + os.path.basename(x) for x in right.split() if x.strip()]
            rewritten.append(f"{l_out}: {' '.join(r_items)}".rstrip() + "\n")
    with open(dep_path, "w", encoding="utf-8") as fh:
        fh.writelines(rewritten)

    # Keep only the runtime files Android modprobe consumes
    for filename in list(os.listdir(part_kdir)):
        if filename.endswith(".ko") or filename in ("modules.dep", "modules.alias", "modules.softdep"):
            continue
        os.remove(os.path.join(part_kdir, filename))

    with open(os.path.join(part_kdir, "modules.load"), "w", encoding="utf-8") as fh:
        for mod in load_order:
            if mod in all_kos:
                fh.write(f"{mod}\n")
    return part_kdir


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage Xiaomi SM8550 kernel modules.")
    parser.add_argument("--mod-stage", required=True)
    parser.add_argument("--common-dir", required=True)
    parser.add_argument("--out-parts", required=True)
    parser.add_argument("--kver", required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()

    kdir = os.path.join(args.mod_stage, "lib", "modules", args.kver)
    all_kos = collect_compiled_modules(kdir)
    deps = build_dep_graph(all_kos, kdir, args.out_parts, args.kver)

    first_stage_load = read_module_list(os.path.join(args.common_dir, "modules.load.first_stage"))
    recovery_load = read_module_list(os.path.join(args.common_dir, "modules.load.recovery"))
    vendor_dlkm_load = read_module_list(os.path.join(args.common_dir, "modules.load.vendor_dlkm"))

    for mod in DEVICE_DLKM_EXTRAS.get(args.device, []):
        if mod not in vendor_dlkm_load:
            vendor_dlkm_load.append(mod)
    for mod in DEVICE_RECOVERY_EXTRAS.get(args.device, []):
        if mod not in recovery_load:
            recovery_load.append(mod)

    fs_mods, fs_missing = expand_with_deps(first_stage_load, all_kos, deps)
    rec_mods, rec_missing = expand_with_deps(recovery_load, all_kos, deps)
    dlkm_mods, dlkm_missing = expand_with_deps(vendor_dlkm_load, all_kos, deps)

    stage_partition("first_stage", fs_mods, first_stage_load, "/lib/modules/", all_kos, kdir, args.out_parts, args.kver)
    stage_partition("recovery", rec_mods, recovery_load, "/lib/modules/", all_kos, kdir, args.out_parts, args.kver)
    stage_partition("vendor_dlkm", dlkm_mods, vendor_dlkm_load, "/vendor_dlkm/lib/modules/", all_kos, kdir, args.out_parts, args.kver)

    report = [
        f"Kernel Release: {args.kver}",
        f"Device: {args.device}",
        f"Total compiled .ko files: {len(all_kos)}",
        f"vendor_boot first_stage: {len(fs_mods)} modules (missing: {len(fs_missing)})",
        f"vendor_boot recovery: {len(rec_mods)} modules (missing: {len(rec_missing)})",
        f"vendor_dlkm: {len(dlkm_mods)} modules (missing: {len(dlkm_missing)})",
    ]
    if fs_missing:
        report.append("Missing first_stage: " + ", ".join(fs_missing))
    if rec_missing:
        report.append("Missing recovery: " + ", ".join(rec_missing))
    if dlkm_missing:
        report.append("Missing vendor_dlkm: " + ", ".join(dlkm_missing))

    text = "\n".join(report) + "\n"
    print(text, end="")
    with open(args.report, "w", encoding="utf-8") as fh:
        fh.write(text)

    if fs_missing or rec_missing or dlkm_missing:
        sys.stderr.write("ERROR: Required modules missing! Aborting package build.\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
