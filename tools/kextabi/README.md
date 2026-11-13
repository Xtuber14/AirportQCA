# kextabi — OpenCore prelinked-injection ABI checker

Static analysis tools that predict whether OpenCore will accept
`AirportQCA.kext` for prelinked injection on a given macOS kernel, without
booting the target machine.

They exist because OpenCore's "Invalid Parameter" error during kext injection
is a *masked* failure: the real error is always emitted just above it in the
log as `OCAK: ...`, and it is almost always a **vtable ABI mismatch** rather
than an OpenCore bug.

## Why the vtable layout matters

OpenCore patches a kext's vtable against the kernel's vtable for the same
superclass **by index**:

* `Library/OcAppleKernelLib/Vtables.c` →
  `InternalInitializeVtableByEntriesAndRelocations` walks
  `Index < SuperVtable->NumEntries` and reads the *child's* vtable data at the
  same index.
* Every child slot that is zero (i.e. inherited, not overridden) is rebound to
  the parent implementation, using the parent's *symbol name*.
* If the parent's slot at that index is a **pad slot** (its name contains
  `_RESERVED`) and the child's symbol has a different name, OpenCore refuses
  the patch (`InternalPatchVtableSymbol`, case 4) and the whole injection
  fails with `Invalid Parameter`.

So a single extra or missing virtual method in the vendored SDK headers shifts
every following slot and breaks the kext. This is the failure mode that broke
`AirportQCA.kext` on Tahoe 26.7.

## Ground truth

The kernel's real vtables are read from
`/System/Library/KernelCollections/BootKernelExtensions.kc`:

* `kclib.py` — MH_FILESET (filetype 12) parser for the KernelCollection.
* `macholib.py` — Mach-O reader: segments, sections, symbols, and the
  `LC_DYSYMTAB` **unified** relocation table (kext relocations live there,
  `r_address` is an absolute VM address).
* `kernel.py` — reproduces OpenCore's exact symbol semantics:
  * `InternalScanBuildLinkedSymbolTable` (C++ symbols are stored backwards,
    so the *last* C++ symbol in symtab order wins a lookup),
  * `InternalOcGetSymbolWorkerValue` (own kext first, then the dependency
    tree),
  * `KcFixupValue` (fixup-encoded values vs. already-absolute kernel
    addresses).

## Tools

| Script | Purpose |
| --- | --- |
| `validate.py` | Runs everything below; the single entry point. |
| `missing.py` | Undefined symbols of the kext vs. the Boot KC. |
| `vtlen.py` | Child vtable length vs. parent length (OpenCore reads the child table using the parent's count). |
| `simulate.py` | Full simulation of OpenCore's vtable patcher (all 6 vtables). |
| `padcheck.py` | The decisive check: per-slot pad-slot mismatch analysis. |
| `test_simulate.py` | Unit tests for the patch-rule logic. |
| `vtdec.py` | Dump a kernel vtable (ground truth) from the Boot KC. |
| `vtbin.py` | Dump a kext vtable from a Mach-O. |

## Usage

```sh
# validate a built kext against the local kernel
python3 tools/kextabi/validate.py \
    build/AirportQCA.kext/Contents/MacOS/AirportQCA

# dump the kernel's vtable for reference
python3 tools/kextabi/vtdec.py __ZTV17IO80211Controller
```

`validate.py` expects the *same* macOS build that will boot the kext: the
kernel's vtables are read from the local `BootKernelExtensions.kc`. Run it on
the machine whose kernel will load the kext.

## Adding an override

If the toolchain reports a vtable mismatch, the fix is **not** in the kext but
in the vendored SDK headers under
`kext/AirportQCA/include/Apple80211/macOS26/`:

* Never add a virtual declaration that the real kernel does not have — it
  shifts every following vtable slot.
* Never remove one that the real kernel does have.
* To fill a slot the kernel reserves, declare it with the *kernel's* name via
  an `__asm__` label and give it the matching source name in the base header,
  e.g.

  ```c
  // in the vendored base header
  virtual void _RESERVEDIONetworkController2();
  // in the derived class
  virtual void _RESERVEDIONetworkController2()
      __asm__("__ZN19IONetworkController11outputStartEP18IONetworkInterfacej")
      APPLE_KEXT_OVERRIDE;
  ```

  Clang matches virtual overrides by *declaration name*, not by the `__asm__`
  label, so the base header must declare the method under the same source
  name that the derived class uses.

## References

* `Library/OcAppleKernelLib/Vtables.c` — the vtable patcher.
* `Library/OcMachoLib/CxxSymbols.c` — pad-slot / pure-virtual detection.
* `Library/OcAppleKernelLib/PrelinkedKext.c` — linked symbol table ordering.
* `Library/OcAppleKernelLib/Link.c` — symbol lookup.
* `Library/OcAppleKernelLib/KernelCollection.c` — `KcFixupValue`.
