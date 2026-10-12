/* uefipatch.h

Copyright (c) 2026, UEFITool contributors. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

#ifndef UEFIPATCH_H
#define UEFIPATCH_H

#include <string>
#include <vector>

#include "../common/basetypes.h"
#include "../common/ubytearray.h"
#include "../common/ustring.h"
#include "../common/treemodel.h"
#include "../common/ffsparser.h"
#include "../common/ffsops.h"
#include "../common/ffsbuilder.h"

// Port of old_engine's UEFIPatch (patches.txt format: "FileGuid SectionType P:find:replace" / "O:hexoffset:replace").
// Patches never change sizes: the patched section body keeps its length, so the image size never changes.
struct UefiPatchData {
    UINT8 type;                 // PATCH_TYPE_PATTERN ('P') or PATCH_TYPE_OFFSET ('O')
    UINT32 offset;              // for 'O' patches
    std::string hexFindPattern;     // for 'P' patches, hex digits and '.' wildcards
    std::string hexReplacePattern;  // hex digits and '.' wildcards (keep the original nibble/byte)
};

// Applies one patch to a section body, as old_engine's FfsEngine::patchViaOffset
USTATUS uefiPatchViaOffset(UByteArray & data, const UINT32 offset, const std::string & hexReplacePattern, std::vector<UString> * messages = NULL);
// Applies a pattern patch to a section body, as old_engine's FfsEngine::patchViaPattern
USTATUS uefiPatchViaPattern(UByteArray & data, const std::string & hexFindPattern, const std::string & hexReplacePattern, std::vector<UString> * messages = NULL);

class UEFIPatch
{
public:
    UEFIPatch() {}
    // Parses the image, applies every patch line from patchText (patches.txt content), rebuilds and returns the image in rebuilt.
    // Returns U_NOTHING_TO_PATCH when the rebuilt image equals the input. messages receives parser/ops/builder/patch messages.
    USTATUS patch(const UByteArray & image, const std::string & patchText, UByteArray & rebuilt, std::vector<UString> & messages);

private:
    USTATUS patchSections(TreeModel * model, FfsOperations * ops, const UModelIndex & index, const UByteArray & fileGuid,
                          const UINT8 sectionType, const std::vector<UefiPatchData> & patches, std::vector<UString> & messages);
    USTATUS patchSection(TreeModel * model, FfsOperations * ops, const UModelIndex & index,
                         const std::vector<UefiPatchData> & patches, std::vector<UString> & messages);
};

#endif
