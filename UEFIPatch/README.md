# UEFIPatch (NE port)

Port of old_engine's UEFIPatch on top of the NE parser and the size-preserving builder. No Qt.

    uefipatch image_file [patches.txt] [-o output]
    uefipatch image_file [-p "Guid SectionType Patch"] [-o output]

Format is the old_engine `patches.txt` format: `FileGuid SectionType P:findhex:replacehex` or `O:hexoffset:replacehex`,
`.` is a wildcard nibble in find/replace patterns, `#` starts a comment. Default output is `image_file.patched`.
Exit code 52 means "nothing to patch" (no match, or result identical to input), no output is written.

Differences from old_engine (see also the commit messages):
* Find patterns accept only hex digits and `.` (old_engine passed them to QRegExp). Matching semantics (case-insensitive,
  odd nibble offsets skipped, matching on the original body while replacing in a copy) are kept.
* Invalid GUID / section type > FF / malformed `P:`/`O:` fields abort with INVALID_PARAMETER (old_engine used a null GUID, truncated, or crashed).
* An offset patch longer than the body is rejected as out of bounds (old_engine's UINT32 check could wrap).
* The patched section is changed in place and marked Replace, parents get Rebuild (old_engine created a new node and removed the old one).
* Patches are same-size by construction; rebuild is refused if the output size would differ or if the builder needs volume growth/rebase.
* Like old_engine, at most one section per parent is patched per patch line (the sibling loop breaks after a success).
* Not ported: Boot Guard / BIOS Guard / FIT hash updates (nothing recomputes them). Do not flash without a programmer + backup.

Tests: `UEFIReplace/tests/ffsedit_test.cpp` T10-T16 (`uefireplace_test image.bin`).
