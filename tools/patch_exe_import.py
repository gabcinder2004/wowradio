"""Add an import of WowRadio.dll to a WoW 3.3.5a executable.

The game only loads DLLs named in its import table, so this is how the radio
gets loaded: one extra entry in that table. It is the same kind of change a
private server already makes to its executable, and no Blizzard file is
replaced. The DLL's imported symbol (WowRadio_Loaded) exists only so the
import has something to bind to; the DLL does its work in DllMain.

Usage:
    python patch_exe_import.py <input exe> [output exe]

The output defaults to <input>_radio.exe. Re-running on an already patched
exe is a no-op. Requires the `lief` package (pip install lief).
"""
import shutil
import sys

import lief

DLL = "WowRadio.dll"
SYMBOL = "WowRadio_Loaded"


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + "_radio.exe"

    pe = lief.parse(src)
    if any(imp.name.lower() == DLL.lower() for imp in pe.imports):
        print(f"{src} already imports {DLL}; nothing to do.")
        if dst != src:
            shutil.copyfile(src, dst)
        return

    lib = pe.add_import(DLL)
    lib.add_entry(SYMBOL)

    # Rebuild only the import table; every other section is written back as-is.
    config = lief.PE.Builder.config_t()
    config.imports = True
    builder = lief.PE.Builder(pe, config)
    builder.build()
    builder.write(dst)
    print(f"Wrote {dst} importing {DLL}!{SYMBOL}")

    names = [imp.name for imp in lief.parse(dst).imports]
    assert any(n.lower() == DLL.lower() for n in names), "import not present after build"
    print("Verified imports:", ", ".join(names))


if __name__ == "__main__":
    main()
