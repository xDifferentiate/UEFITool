# UEFIReplace (NE, experimental, size-preserving)

Work in progress port of the old_engine `UEFIReplace` to the NE code base. Not an official upstream feature.

    uefireplace image_file guid section_type contents_file [-o output] [-all] [-asis]

Build (no Qt needed): `cmake -S UEFIReplace -B build && make -C build` -> `build/uefireplace`, `build/uefireplace_test`.

## What it does
* Parses the image with the NE parser, replaces the matching section (or file, if `section_type` is a file type) body (or
  whole item with `-asis`), rebuilds only the changed chain (section -> file -> volume -> parents) and writes the image.
* Rebuilt: section sizes, `UncompressedLength`, recompression (EFI 1.1, Tiano, LZMA, LZMAF86), CRC32 GUID-defined
  sections, FFS header/data checksums (+ rev1 tail), FV header checksum, pad file regeneration.
* Untouched items are copied byte-for-byte.

## Hard limits (refused with an error, no output written)
* The image/volume size never changes. Size changes are absorbed by following pad files / volume free space; files
  directly behind the edited one may be shifted only if they are not PEI-phase XIP files, not Fixed, not the VTF and
  have no alignment above 8 bytes. Otherwise `Not enough free space in volume` is reported.
* Modifying PE32/TE/PIC of PEI-phase files in uncompressed volumes (needs rebase): refused.
* Apple CRC32 volumes, volumes with a valid UsedSpace that would change, non-FFS volumes (NVRAM etc.), compressed
  GUID-defined sections other than LZMA/LZMAF86 (Tiano/GZip/Zlib/Brotli) after a content change, Intel-legacy LZMA.
* Nothing updates Boot Guard / BIOS Guard / FIT / vendor hash ranges. An edited image may fail verification on hardware.

## Tests
`build/uefireplace_test image.bin [outdir]` runs identity rebuild, forced-full rebuild, same-content replace, same-size edit,
shrink, small growth, oversize refusal, PE/TE-rebase refusal and an EFI 1.1 recompression edit. T3-T9 use GUIDs specific
to the Cyborg-STOCK image.
