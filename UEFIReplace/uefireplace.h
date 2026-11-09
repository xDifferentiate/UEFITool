/* uefireplace.h

Copyright (c) 2026, UEFITool contributors. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

#ifndef UEFIREPLACE_H
#define UEFIREPLACE_H

#include <vector>

#include "../common/basetypes.h"
#include "../common/ubytearray.h"
#include "../common/ustring.h"
#include "../common/treemodel.h"
#include "../common/ffsparser.h"
#include "../common/ffsops.h"
#include "../common/ffsbuilder.h"

// Replaces matching items in an already parsed tree. Matching follows old_engine's UEFIReplace:
// item subtype == sectionType, and the GUID of the parent file (or, for freeform subtype GUID
// sections, the section's subtype GUID) equals guid. Returns U_NOTHING_TO_PATCH if nothing matched.
USTATUS uefiReplaceInTree(TreeModel * model, FfsOperations * ops, const UModelIndex & index, const UByteArray & guid,
                          const UINT8 sectionType, UByteArray & newData, const UINT8 mode, const bool replaceOnce);

class UEFIReplace
{
public:
    UEFIReplace() {}
    // Parses the image, applies the replacement, rebuilds the image (same size only) and returns it in rebuilt.
    // messages receives parser/ops/builder messages. When the rebuilt image equals the input, U_NOTHING_TO_PATCH is returned.
    USTATUS replace(const UByteArray & image, const UByteArray & guid, const UINT8 sectionType, UByteArray & contents,
                    const bool asIs, const bool replaceOnce, UByteArray & rebuilt, std::vector<UString> & messages);
};

#endif
