/* fssops.cpp
 
 Copyright (c) 2015, Nikolaj Schlej. All rights reserved.
 This program and the accompanying materials
 are licensed and made available under the terms and conditions of the BSD License
 which accompanies this distribution.  The full text of the license may be found at
 http://opensource.org/licenses/bsd-license.php
 
 THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
 
 */

#include "ffsops.h"
#include "ffs.h"
#include "utility.h"

#include <algorithm>

USTATUS FfsOperations::extract(const UModelIndex & index, UString & name, UByteArray & extracted, const UINT8 mode)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // Default name
    name = uniqueItemName(index);
    
    // Get extracted data
    if (mode == EXTRACT_MODE_AS_IS) {
        // Extract as is, with header body and tail
        extracted.clear();
        extracted += model->full(index);
    }
    else if (mode == EXTRACT_MODE_BODY) {
        name += UString("_body");
        // Extract without header and tail
        extracted.clear();
        extracted += model->body(index);
    }
    else if (mode == EXTRACT_MODE_UNCOMPRESSED) {
        name += UString("_uncompressed");
        extracted.clear();
        extracted += model->uncompressedData(index);
    }
    else
        return U_UNKNOWN_EXTRACT_MODE;
    
    return U_SUCCESS;
}

// Size of the section header (common header + type-specific part), same layout the parser uses
static USTATUS sectionHeaderSizeOf(const UByteArray & section, UINT32 & headerSize)
{
    if ((UINT32)section.size() < sizeof(EFI_COMMON_SECTION_HEADER))
        return U_INVALID_SECTION;
    const EFI_COMMON_SECTION_HEADER* common = (const EFI_COMMON_SECTION_HEADER*)section.constData();
    UINT32 base = sizeof(EFI_COMMON_SECTION_HEADER);
    if (uint24ToUint32(common->Size) == EFI_SECTION2_IS_USED)
        base = sizeof(EFI_COMMON_SECTION_HEADER2);
    UINT32 extra = 0;
    switch (common->Type) {
        case EFI_SECTION_COMPRESSION:        extra = sizeof(EFI_COMPRESSION_SECTION); break;
        case EFI_SECTION_GUID_DEFINED: {
            if ((UINT32)section.size() < base + sizeof(EFI_GUID_DEFINED_SECTION))
                return U_INVALID_SECTION;
            const EFI_GUID_DEFINED_SECTION* g = (const EFI_GUID_DEFINED_SECTION*)(section.constData() + base);
            // Header ends at DataOffset (which counts from the start of the section)
            headerSize = std::max<UINT32>(g->DataOffset, base + sizeof(EFI_GUID_DEFINED_SECTION));
            return (UINT32)section.size() < headerSize ? U_INVALID_SECTION : U_SUCCESS;
        }
        case EFI_SECTION_FREEFORM_SUBTYPE_GUID: extra = sizeof(EFI_FREEFORM_SUBTYPE_GUID_SECTION); break;
        case EFI_SECTION_VERSION:            extra = sizeof(EFI_VERSION_SECTION); break;
        case PHOENIX_SECTION_POSTCODE:
        case INSYDE_SECTION_POSTCODE:        extra = sizeof(POSTCODE_SECTION); break;
        default: break;
    }
    headerSize = base + extra;
    return (UINT32)section.size() < headerSize ? U_INVALID_SECTION : U_SUCCESS;
}

USTATUS FfsOperations::replace(const UModelIndex & index, UByteArray & data, const UINT8 mode)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;

    if (mode != REPLACE_MODE_AS_IS && mode != REPLACE_MODE_BODY)
        return U_UNKNOWN_REPLACE_MODE;

    const UINT8 type = model->type(index);
    if (type == Types::Section) {
        if (mode == REPLACE_MODE_AS_IS) {
            UINT32 headerSize = 0;
            USTATUS result = sectionHeaderSizeOf(data, headerSize);
            if (result) {
                msg(UString("replace: supplied data does not start with a valid section header"), index);
                return result;
            }
            // Refuse to silently change the section type
            if (((const EFI_COMMON_SECTION_HEADER*)data.constData())->Type != model->subtype(index)) {
                msg(UString("replace: section type of the supplied data differs from the replaced section"), index);
                return U_INVALID_SECTION;
            }
            model->setHeader(index, data.left((int)headerSize));
            model->setBody(index, data.mid((int)headerSize));
            model->setTail(index, UByteArray());
        }
        else {
            model->setBody(index, data);
        }
    }
    else if (type == Types::File) {
        if (model->subtype(index) == EFI_FV_FILETYPE_PAD) {
            msg(UString("replace: replacing pad files is not supported"), index);
            return U_EDIT_NOT_SUPPORTED;
        }
        if (mode == REPLACE_MODE_AS_IS) {
            // Header size is taken from the original file: large-file flag must not change
            UINT32 headerSize = model->headerSize(index);
            if ((UINT32)data.size() < headerSize
                || (((const EFI_FFS_FILE_HEADER*)data.constData())->Attributes & FFS_ATTRIB_LARGE_FILE)
                   != (((const EFI_FFS_FILE_HEADER*)model->header(index).constData())->Attributes & FFS_ATTRIB_LARGE_FILE)) {
                msg(UString("replace: supplied data is not a file with the same header layout as the replaced file"), index);
                return U_INVALID_FILE;
            }
            UByteArray body = data.mid((int)headerSize);
            UINT32 tailSize = model->tailSize(index);
            model->setHeader(index, data.left((int)headerSize));
            if (tailSize && (UINT32)body.size() >= tailSize) {
                model->setTail(index, body.right((int)tailSize));
                body = body.left((int)(body.size() - tailSize));
            }
            model->setBody(index, body);
        }
        else {
            model->setBody(index, data);
        }
    }
    else {
        msg(UString("replace: only File and Section items can be replaced"), index);
        return U_EDIT_NOT_SUPPORTED;
    }

    // Mark the item as replaced: its children (if any) are not used by the builder anymore
    model->setAction(index, Actions::Replace);
    // Parents must be rebuilt
    UModelIndex parent = index.parent();
    if (parent.isValid() && model->type(parent) != Types::Root && model->action(parent) == Actions::NoAction)
        rebuild(parent);
    return U_SUCCESS;
}

USTATUS FfsOperations::remove(const UModelIndex & index)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // Set remove action
    model->setAction(index, Actions::Remove);
    
    return U_SUCCESS;
}

USTATUS FfsOperations::rebuild(const UModelIndex & index)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // On insert action, set insert action for children
    //if (action == Actions::Insert)
    //    for (int i = 0; i < item->childCount(); i++)
    //        setAction(index.child(i, 0), Actions::Insert);
    
    // Set rebuild action
    model->setAction(index, Actions::Rebuild);
    
    // Rebuild parent, if it has no action now
    UModelIndex parent = index.parent();
    if (parent.isValid() && model->type(parent) != Types::Root
        && model->action(parent) == Actions::NoAction)
        rebuild(parent);
    
    return U_SUCCESS;
}
