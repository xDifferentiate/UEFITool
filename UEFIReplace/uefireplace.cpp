/* uefireplace.cpp

Copyright (c) 2026, UEFITool contributors. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

#include "uefireplace.h"
#include "../common/ffs.h"
#include "../common/utility.h"

USTATUS uefiReplaceInTree(TreeModel * model, FfsOperations * ops, const UModelIndex & index, const UByteArray & guid,
                          const UINT8 sectionType, UByteArray & newData, const UINT8 mode, const bool replaceOnce)
{
    if (!model || !index.isValid())
        return U_INVALID_PARAMETER;

    bool patched = false;
    const UINT8 type = model->type(index);
    if ((type == Types::File || type == Types::Section) && model->subtype(index) == sectionType) {
        UModelIndex fileIndex = (type == Types::File) ? index : model->findParentOfType(index, Types::File);
        UByteArray fileGuid;
        if (fileIndex.isValid())
            fileGuid = model->header(fileIndex).left(sizeof(EFI_GUID));

        bool guidMatch = (fileGuid == guid);
        if (!guidMatch && type == Types::Section && sectionType == EFI_SECTION_FREEFORM_SUBTYPE_GUID) {
            UByteArray subGuid = model->header(index).mid(sizeof(UINT32), sizeof(EFI_GUID));
            guidMatch = (subGuid == guid);
        }
        if (guidMatch && model->action(index) != Actions::Replace) {
            USTATUS result = ops->replace(index, newData, mode);
            if (result != U_SUCCESS)
                return result;
            patched = true;
            if (replaceOnce)
                return U_SUCCESS;
            // The replaced item's old children are dropped by the builder, don't look inside
            return U_SUCCESS;
        }
    }

    for (int i = 0; i < model->rowCount(index); i++) {
        USTATUS result = uefiReplaceInTree(model, ops, index.model()->index(i, 0, index), guid, sectionType, newData, mode, replaceOnce);
        if (result == U_SUCCESS) {
            patched = true;
            if (replaceOnce)
                break;
        }
        else if (result != U_NOTHING_TO_PATCH) {
            return result;
        }
    }
    return patched ? U_SUCCESS : U_NOTHING_TO_PATCH;
}

USTATUS UEFIReplace::replace(const UByteArray & image, const UByteArray & guid, const UINT8 sectionType, UByteArray & contents,
                             const bool asIs, const bool replaceOnce, UByteArray & rebuilt, std::vector<UString> & messages)
{
    TreeModel model;
    FfsParser parser(&model);
    USTATUS result = parser.parse(image);
    if (result)
        return result;

    FfsOperations ops(&model);
    result = uefiReplaceInTree(&model, &ops, model.index(0, 0), guid, sectionType, contents,
                               asIs ? REPLACE_MODE_AS_IS : REPLACE_MODE_BODY, replaceOnce);
    std::vector<std::pair<UString, UModelIndex> > opsMessages = ops.getMessages();
    for (size_t i = 0; i < opsMessages.size(); i++)
        messages.push_back(opsMessages[i].first);
    if (result)
        return result;

    FfsBuilder builder(&model);
    rebuilt.clear();
    result = builder.build(model.index(0, 0), rebuilt);
    std::vector<std::pair<UString, UModelIndex> > builderMessages = builder.getMessages();
    for (size_t i = 0; i < builderMessages.size(); i++)
        messages.push_back(builderMessages[i].first);
    if (result) {
        rebuilt.clear();
        return result;
    }

    if (rebuilt.size() != image.size()) {
        messages.push_back(UString("rebuilt image size differs from the input image size, refusing to produce output"));
        rebuilt.clear();
        return U_INVALID_IMAGE;
    }
    if (rebuilt == image)
        return U_NOTHING_TO_PATCH;
    return U_SUCCESS;
}
