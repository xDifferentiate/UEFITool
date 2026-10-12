/* fssbuilder.cpp
 
 Copyright (c) 2015, Nikolaj Schlej. All rights reserved.
 This program and the accompanying materials
 are licensed and made available under the terms and conditions of the BSD License
 which accompanies this distribution.  The full text of the license may be found at
 http://opensource.org/licenses/bsd-license.php
 
 THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
 
 */
#include "ffsbuilder.h"

#include "descriptor.h"
#include "ffs.h"
#include "peimage.h"
#include "utility.h"
#include "nvram.h"

#include <cstring>
#include <vector>

#include "Tiano/EfiTianoCompress.h"
#include "LZMA/LzmaCompress.h"
#include "LZMA/SDK/C/Bra.h"
#include "zlib/zlib.h"

USTATUS FfsBuilder::erase(const UModelIndex & index, UByteArray & erased)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // Try to get emptyByte value from item's parsing data
    UINT8 emptyByte = 0xFF;
    if (!model->hasEmptyParsingData(index)) {
        if (model->type(index) == Types::Volume) {
            VOLUME_PARSING_DATA pdata = *(VOLUME_PARSING_DATA*)model->parsingData(index).constData();
            emptyByte = pdata.emptyByte;
        }
        else if (model->type(index) == Types::File) {
            FILE_PARSING_DATA pdata = *(FILE_PARSING_DATA*)model->parsingData(index).constData();
            emptyByte = pdata.emptyByte;
        }
    }
    
    erased = UByteArray(model->fullSize(index), emptyByte);
    
    return U_SUCCESS;
}

USTATUS FfsBuilder::build(const UModelIndex & root, UByteArray & image)
{
    // Sanity check
    if (!root.isValid())
        return U_INVALID_PARAMETER;
    
    if (model->type(root) == Types::Capsule) {
        return buildCapsule(root, image);
    }
    else if (model->type(root) == Types::Image) {
        if (model->subtype(root) == Subtypes::IntelImage) {
            return buildIntelImage(root, image);
        }
        else if (model->subtype(root) == Subtypes::UefiImage) {
            return buildRawArea(root, image);
        }
    }
    
    return U_NOT_IMPLEMENTED;
}

USTATUS FfsBuilder::buildCapsule(const UModelIndex & index, UByteArray & capsule)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // No action
    if (model->action(index) == Actions::NoAction) {
        // Use original item data
        capsule = model->full(index);
        return U_SUCCESS;
    }
    
    // Rebuild or Replace
    else if (model->action(index) == Actions::Rebuild
             || model->action(index) == Actions::Replace) {
        if (model->rowCount(index)) {
            // Clear the supplied UByteArray
            capsule.clear();
            
            // Right now there is only one capsule image element supported
            if (model->rowCount(index) != 1) {
                msg(usprintf("buildCapsule: building of capsules with %d items is not yet supported", model->rowCount(index)), index);
                return U_NOT_IMPLEMENTED;
            }
            
            // Build image
            UModelIndex imageIndex = index.model()->index(0, 0, index);
            UByteArray imageData;
            
            // Check image type
            if (model->type(imageIndex) == Types::Image) {
                USTATUS result = U_SUCCESS;
                if (model->subtype(imageIndex) == Subtypes::IntelImage) {
                    result = buildIntelImage(imageIndex, imageData);
                }
                else if (model->subtype(imageIndex) == Subtypes::UefiImage) {
                    result = buildRawArea(imageIndex, imageData);
                }
                else {
                    msg(UString("buildCapsule: unexpected item subtype ") + itemSubtypeToUString(model->type(imageIndex), model->subtype(imageIndex)), imageIndex);
                    return U_UNKNOWN_ITEM_TYPE;
                }
                
                // Check build result
                if (result) {
                    msg(UString("buildCapsule: building of ") + model->name(imageIndex) + UString(" failed with error ") + errorCodeToUString(result), imageIndex);
                    return result;
                }
                else
                    capsule += imageData;
            }
            else {
                msg(UString("buildCapsule: unexpected item type ") + itemTypeToUString(model->type(imageIndex)), imageIndex);
                return U_UNKNOWN_ITEM_TYPE;
            }
            
            // Check size of reconstructed capsule body, it must remain the same
            UINT32 newSize = (UINT32)capsule.size();
            UINT32 oldSize = model->bodySize(index);
            if (newSize > oldSize) {
                msg(usprintf("buildCapsule: new capsule size %Xh (%u) is bigger than the original %Xh (%u)", newSize, newSize, oldSize, oldSize), index);
                return U_INVALID_CAPSULE;
            }
            else if (newSize < oldSize) {
                msg(usprintf("buildCapsule: new capsule size %Xh (%u) is smaller than the original %Xh (%u)", newSize, newSize, oldSize, oldSize), index);
                return U_INVALID_CAPSULE;
            }
        }
        else
            capsule = model->body(index);
        
        // Build successful, append header and tail
        capsule = model->header(index) + capsule + model->tail(index);
        return U_SUCCESS;
    }
    
    msg(UString("buildCapsule: unexpected action " + actionTypeToUString(model->action(index))), index);
    return U_NOT_IMPLEMENTED;
}

USTATUS FfsBuilder::buildIntelImage(const UModelIndex & index, UByteArray & intelImage)
{
    // Sanity check
    if (!index.isValid())
        return U_SUCCESS;
    
    // No action
    if (model->action(index) == Actions::NoAction) {
        intelImage = model->full(index);
        return U_SUCCESS;
    }
    // Remove
    else if (model->action(index) == Actions::Remove) {
        intelImage.clear();
        return U_SUCCESS;
    }
    // Rebuild
    else if (model->action(index) == Actions::Rebuild) {
        // First child will always be descriptor for this type of image, and it's read only for now
        intelImage = model->full(index.model()->index(0, 0, index));
        
        // Process other regions
        for (int i = 1; i < model->rowCount(index); i++) {
            UModelIndex currentRegion = index.model()->index(i, 0, index);
            
            // Skip regions with Remove action
            if (model->action(currentRegion) == Actions::Remove)
                continue;
            
            // Check item type to be either region or padding
            UINT8 type = model->type(currentRegion);
            if (type == Types::Padding) {
                // Add padding as is
                intelImage += model->full(currentRegion);
                continue;
            }
            
            // Check region subtype
            USTATUS result;
            UByteArray region;
            UINT8 regionType = model->subtype(currentRegion);
            switch (regionType) {
                case Subtypes::BiosRegion:
                case Subtypes::PdrRegion:
                    result = buildRawArea(currentRegion, region);
                    if (result) {
                        msg(UString("buildIntelImage: building of region ") + regionTypeToUString(regionType) + UString(" failed with error ") + errorCodeToUString(result), currentRegion);
                        return result;
                    }
                    break;
                case Subtypes::MeRegion:
                case Subtypes::GbeRegion:
                case Subtypes::DevExp1Region:
                case Subtypes::Bios2Region:
                case Subtypes::MicrocodeRegion:
                case Subtypes::EcRegion:
                case Subtypes::DevExp2Region:
                case Subtypes::IeRegion:
                case Subtypes::Tgbe1Region:
                case Subtypes::Tgbe2Region:
                case Subtypes::Reserved1Region:
                case Subtypes::Reserved2Region:
                case Subtypes::PttRegion:
                    // Add region as is
                    region = model->header(currentRegion) + model->body(currentRegion);
                    break;
                default:
                    msg(UString("buildIntelImage: unknown region type"), currentRegion);
                    return U_UNKNOWN_ITEM_TYPE;
            }
            
            // Append the resulting region
            intelImage += region;
        }
        
        // Check size of new image, it must be same as old one
        UINT32 newSize = (UINT32)intelImage.size();
        UINT32 oldSize = model->bodySize(index);
        if (newSize > oldSize) {
            msg(usprintf("buildIntelImage: new image size %Xh (%u) is bigger than the original %Xh (%u)", newSize, newSize, oldSize, oldSize), index);
            return U_INVALID_IMAGE;
        }
        else if (newSize < oldSize) {
            msg(usprintf("buildIntelImage: new image size %Xh (%u) is smaller than the original %Xh (%u)", newSize, newSize, oldSize, oldSize), index);
            return U_INVALID_IMAGE;
        }
        
        // Build successful, append header and tail
        intelImage = model->header(index) + intelImage + model->tail(index);
        return U_SUCCESS;
    }
    
    msg(UString("buildIntelImage: unexpected action " + actionTypeToUString(model->action(index))), index);
    return U_NOT_IMPLEMENTED;
}

USTATUS FfsBuilder::buildRawArea(const UModelIndex & index, UByteArray & rawArea)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // No action required
    if (model->action(index) == Actions::NoAction) {
        rawArea = model->full(index);
        return U_SUCCESS;
    }
    // Remove
    else if (model->action(index) == Actions::Remove) {
        rawArea.clear();
        return U_SUCCESS;
    }
    // Rebuild or Replace
    else if (model->action(index) == Actions::Rebuild
             || model->action(index) == Actions::Replace) {
        // Rebuild if there is at least 1 child
        if (model->rowCount(index)) {
            // Clear the supplied UByteArray
            rawArea.clear();
            
            // Build children
            for (int i = 0; i < model->rowCount(index); i++) {
                USTATUS result = U_SUCCESS;
                UModelIndex currentChild = index.model()->index(i, 0, index);
                UByteArray currentData;
                
                // Check child type
                if (model->type(currentChild) == Types::Volume) {
                    result = buildVolume(currentChild, currentData);
                }
                else if (model->type(currentChild) == Types::Padding) {
                    result = buildPadding(currentChild, currentData);
                }
                else {
                    msg(UString("buildRawArea: unexpected item type ") + itemTypeToUString(model->type(currentChild)), currentChild);
                    return U_UNKNOWN_ITEM_TYPE;
                }
                // Check build result
                if (result) {
                    msg(UString("buildRawArea: building of ") + model->name(currentChild) + UString(" failed with error ") + errorCodeToUString(result), currentChild);
                    return result;
                }
                // Append current data
                rawArea += currentData;
            }
            
            // Check size of new raw area, it must be same as original one
            UINT32 newSize = (UINT32)rawArea.size();
            UINT32 oldSize = model->bodySize(index);
            if (newSize > oldSize) {
                msg(usprintf("buildRawArea: new area size %Xh (%u) is bigger than the original %Xh (%u)", newSize, newSize, oldSize, oldSize), index);
                return U_INVALID_RAW_AREA;
            }
            else if (newSize < oldSize) {
                msg(usprintf("buildRawArea: new area size %Xh (%u) is smaller than the original %Xh (%u)", newSize, newSize, oldSize, oldSize), index);
                return U_INVALID_RAW_AREA;
            }
        }
        // No need to rebuild a raw area with no children
        else {
            rawArea = model->body(index);
        }
        
        // Build successful, add header if needed
        rawArea = model->header(index) + rawArea + model->tail(index);
        return U_SUCCESS;
    }
    
    msg(UString("buildRawArea: unexpected action " + actionTypeToUString(model->action(index))), index);
    return U_NOT_IMPLEMENTED;
}

USTATUS FfsBuilder::buildPadding(const UModelIndex & index, UByteArray & padding)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // No action required
    if (model->action(index) == Actions::NoAction) {
        padding = model->full(index);
        return U_SUCCESS;
    }
    // Remove
    else if (model->action(index) == Actions::Remove) {
        padding.clear();
        return U_SUCCESS;
    }
    // Erase
    else if (model->action(index) == Actions::Erase) {
        return erase(index, padding);
    }
    
    msg(UString("buildPadding: unexpected action " + actionTypeToUString(model->action(index))), index);
    return U_NOT_IMPLEMENTED;
}

USTATUS FfsBuilder::buildNonUefiData(const UModelIndex & index, UByteArray & data)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // No action required
    if (model->action(index) == Actions::NoAction) {
        data = model->full(index);
        return U_SUCCESS;
    }
    // Remove
    else if (model->action(index) == Actions::Remove) {
        data.clear();
        return U_SUCCESS;
    }
    // Erase
    else if (model->action(index) == Actions::Erase) {
        return erase(index, data);
    }
    
    msg(UString("buildNoUefiData: unexpected action " + actionTypeToUString(model->action(index))), index);
    return U_NOT_IMPLEMENTED;
}

USTATUS FfsBuilder::buildFreeSpace(const UModelIndex & index, UByteArray & freeSpace)
{
    // Sanity check
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    
    // No actions possible for free space
    freeSpace = model->full(index);
    return U_SUCCESS;
}


// ---------------------------------------------------------------------------------------------
// Size-preserving builder: Volume / File / Section reconstruction.
//
// Design rules (see README section "Editing"):
//  * Items with NoAction are emitted byte-for-byte from the parsed image.
//  * Items with Replace action are emitted from the header/body/tail stored in the tree item
//    (children are ignored), with size fields and checksums fixed up.
//  * Items with Rebuild action are rebuilt from their children, with size fields and checksums recomputed.
//  * A volume never changes its size. Files keep their offsets unless the file in front of them changed
//    size, in which case the difference is absorbed by directly following pad file / free space.
//    If that is not possible the build fails with U_VOLUME_FULL.
//  * Nothing is rebased: edits needing PE/TE relocation (PE32/TE/PIC sections of PEI-phase files
//    in uncompressed volumes) are refused with U_REBASE_REQUIRED.
// ---------------------------------------------------------------------------------------------

namespace {

// Compression of section data, mirrors the algorithms that decompress() supports
USTATUS compressData(const UByteArray & data, const UINT8 algorithm, const UINT32 dictionarySize, UByteArray & compressed)
{
    switch (algorithm) {
    case COMPRESSION_ALGORITHM_NONE:
        compressed = data;
        return U_SUCCESS;
    case COMPRESSION_ALGORITHM_EFI11:
    case COMPRESSION_ALGORITHM_TIANO: {
        const bool tiano = (algorithm == COMPRESSION_ALGORITHM_TIANO);
        // Try legacy function first, check that the result decompresses back to the source, then fall back to the current one
        for (int legacy = 1; legacy >= 0; legacy--) {
            UINT32 compressedSize = 0;
            auto fn = tiano ? (legacy ? TianoCompressLegacy : TianoCompress) : (legacy ? EfiCompressLegacy : EfiCompress);
            if (fn(data.constData(), (UINT32)data.size(), NULL, &compressedSize) != U_BUFFER_TOO_SMALL)
                return U_STANDARD_COMPRESSION_FAILED;
            std::vector<UINT8> buf(compressedSize);
            if (fn(data.constData(), (UINT32)data.size(), buf.data(), &compressedSize) != U_SUCCESS)
                return U_STANDARD_COMPRESSION_FAILED;
            compressed = UByteArray((const char*)buf.data(), (int)compressedSize);
            if (!legacy)
                return U_SUCCESS; // current functions are trusted, as in old_engine
            UINT8 algo = 0; UINT32 dict = 0; UByteArray dec, efiDec;
            if (decompress(compressed, EFI_STANDARD_COMPRESSION, algo, dict, dec, efiDec) == U_SUCCESS) {
                if (tiano && (algo == COMPRESSION_ALGORITHM_TIANO || algo == COMPRESSION_ALGORITHM_UNDECIDED) && dec == data)
                    return U_SUCCESS;
                if (!tiano && ((algo == COMPRESSION_ALGORITHM_EFI11 && dec == data) || (algo == COMPRESSION_ALGORITHM_UNDECIDED && efiDec == data)))
                    return U_SUCCESS;
            }
        }
        return U_STANDARD_COMPRESSION_FAILED;
    }
    case COMPRESSION_ALGORITHM_LZMA:
    case COMPRESSION_ALGORITHM_LZMAF86: {
        UByteArray src = data;
        if (algorithm == COMPRESSION_ALGORITHM_LZMAF86 && src.size() > 0) {
            UINT32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
            z7_BranchConvSt_X86_Enc((Byte*)src.data(), (SizeT)src.size(), 0, &state);
        }
        UINT32 compressedSize = 0;
        if (LzmaCompress((const UINT8*)src.constData(), (UINT32)src.size(), NULL, &compressedSize, dictionarySize) != U_BUFFER_TOO_SMALL)
            return U_CUSTOMIZED_COMPRESSION_FAILED;
        std::vector<UINT8> buf(compressedSize);
        if (LzmaCompress((const UINT8*)src.constData(), (UINT32)src.size(), buf.data(), &compressedSize, dictionarySize) != U_SUCCESS)
            return U_CUSTOMIZED_COMPRESSION_FAILED;
        compressed = UByteArray((const char*)buf.data(), (int)compressedSize);
        return U_SUCCESS;
    }
    default:
        return U_UNKNOWN_COMPRESSION_ALGORITHM;
    }
}

// Sets the total size of a section into its (common or extended) header
USTATUS setSectionSize(UByteArray & header, const UINT32 totalSize)
{
    if ((UINT32)header.size() < sizeof(EFI_COMMON_SECTION_HEADER))
        return U_INVALID_SECTION;
    EFI_COMMON_SECTION_HEADER* common = (EFI_COMMON_SECTION_HEADER*)header.data();
    if (uint24ToUint32(common->Size) == EFI_SECTION2_IS_USED) {
        if ((UINT32)header.size() < sizeof(EFI_COMMON_SECTION_HEADER2))
            return U_INVALID_SECTION;
        ((EFI_COMMON_SECTION_HEADER2*)header.data())->ExtendedSize = totalSize;
    }
    else {
        if (totalSize >= EFI_SECTION2_IS_USED)
            return U_EDIT_NOT_SUPPORTED; // would need to convert the section to extended header form
        uint32ToUint24(totalSize, common->Size);
    }
    return U_SUCCESS;
}

bool isLeafOnlyRawContainer(const UINT8 sectionType)
{
    return sectionType == EFI_SECTION_RAW || sectionType == EFI_SECTION_FIRMWARE_VOLUME_IMAGE
        || sectionType == EFI_SECTION_FREEFORM_SUBTYPE_GUID;
}

UINT32 commonSectionHeaderSize(const UByteArray & header)
{
    const EFI_COMMON_SECTION_HEADER* common = (const EFI_COMMON_SECTION_HEADER*)header.constData();
    return uint24ToUint32(common->Size) == EFI_SECTION2_IS_USED ? (UINT32)sizeof(EFI_COMMON_SECTION_HEADER2) : (UINT32)sizeof(EFI_COMMON_SECTION_HEADER);
}

// Recomputes FFS file header checksum (State excluded, per the parser's check) and data checksum
void fixFileChecksums(UByteArray & header, const UByteArray & body, const UINT8 volumeRevision)
{
    EFI_FFS_FILE_HEADER* fh = (EFI_FFS_FILE_HEADER*)header.data();
    fh->IntegrityCheck.Checksum.Header = 0;
    fh->IntegrityCheck.Checksum.File = 0;
    fh->IntegrityCheck.Checksum.Header = (UINT8)(0x100 - (calculateSum8((const UINT8*)header.constData(), (UINT32)header.size()) - fh->State));
    if (fh->Attributes & FFS_ATTRIB_CHECKSUM)
        fh->IntegrityCheck.Checksum.File = body.size() ? calculateChecksum8((const UINT8*)body.constData(), (UINT32)body.size()) : 0;
    else
        fh->IntegrityCheck.Checksum.File = (volumeRevision == 1) ? FFS_FIXED_CHECKSUM : FFS_FIXED_CHECKSUM2;
}

} // namespace

bool FfsBuilder::isModified(const UModelIndex & index) const
{
    if (model->action(index) != Actions::NoAction)
        return true;
    for (int i = 0; i < model->rowCount(index); i++)
        if (isModified(index.model()->index(i, 0, index)))
            return true;
    return false;
}

bool FfsBuilder::isXipPeiFile(const UModelIndex & fileIndex) const
{
    if (model->type(fileIndex) != Types::File)
        return false;
    UINT8 t = model->subtype(fileIndex);
    // Files executed in place by the PEI phase (or SEC) and therefore relocated by position
    if (t != EFI_FV_FILETYPE_SECURITY_CORE && t != EFI_FV_FILETYPE_PEI_CORE
        && t != EFI_FV_FILETYPE_PEIM && t != EFI_FV_FILETYPE_COMBINED_PEIM_DRIVER)
        return false;
    return !model->compressed(fileIndex);
}

USTATUS FfsBuilder::buildChild(const UModelIndex & index, UByteArray & built)
{
    switch (model->type(index)) {
    case Types::Volume:    return buildVolume(index, built);
    case Types::Padding:   return buildPadding(index, built);
    case Types::FreeSpace: return buildFreeSpace(index, built);
    case Types::Section:   return buildSection(index, built);
    case Types::File:
        return model->subtype(index) == EFI_FV_FILETYPE_PAD ? buildPadFile(index, built) : buildFile(index, built);
    default:
        break;
    }
    // Items we don't know how to build are only acceptable if untouched
    if (!isModified(index)) {
        built = model->full(index);
        return U_SUCCESS;
    }
    if (model->action(index) == Actions::Remove) {
        built.clear();
        return U_SUCCESS;
    }
    msg(UString("buildChild: don't know how to rebuild item of type ") + itemTypeToUString(model->type(index)), index);
    return U_EDIT_NOT_SUPPORTED;
}

// Concatenates children of a section/file. Section children are separated by zero alignment bytes to 4,
// original alignment bytes are preserved when the child size didn't change.
USTATUS FfsBuilder::buildChildren(const UModelIndex & index, UByteArray & built, const UINT32 sectionAlignment, std::vector<UINT32> * offsets)
{
    built.clear();
    if (offsets) offsets->clear();
    const int count = model->rowCount(index);
    for (int i = 0; i < count; i++) {
        UModelIndex child = index.model()->index(i, 0, index);
        UByteArray data;
        USTATUS result = buildChild(child, data);
        if (result) {
            msg(UString("buildChildren: building of ") + model->name(child) + UString(" failed with error ") + errorCodeToUString(result), child);
            return result;
        }
        if (offsets) offsets->push_back((UINT32)built.size());
        if (model->action(child) == Actions::Remove)
            continue;
        UINT32 offsetBefore = (UINT32)built.size();
        built += data;
        if (sectionAlignment && model->type(child) == Types::Section) {
            if (data.size() == (int)model->fullSize(child)) {
                built += model->alignmentBytes(child); // untouched size, keep original alignment bytes
            }
            else if (i + 1 < count) {
                UINT32 end = offsetBefore + (UINT32)data.size();
                built += UByteArray((size_t)(((end + sectionAlignment - 1) / sectionAlignment) * sectionAlignment - end), '\x00');
            }
        }
    }
    return U_SUCCESS;
}

USTATUS FfsBuilder::buildPadFile(const UModelIndex & index, UByteArray & padFile)
{
    if (!index.isValid())
        return U_INVALID_PARAMETER;
    if (model->action(index) == Actions::Remove) {
        padFile.clear();
        return U_SUCCESS;
    }
    if (!isModified(index)) {
        padFile = model->full(index);
        return U_SUCCESS;
    }
    // Pad files are only (re)created by the volume layout code, never edited directly
    msg(UString("buildPadFile: editing of pad files is not supported"), index);
    return U_EDIT_NOT_SUPPORTED;
}

USTATUS FfsBuilder::buildFile(const UModelIndex & index, UByteArray & file)
{
    if (!index.isValid())
        return U_INVALID_PARAMETER;

    if (model->action(index) == Actions::NoAction) {
        file = model->full(index);
        return U_SUCCESS;
    }
    if (model->action(index) == Actions::Remove) {
        file.clear();
        return U_SUCCESS;
    }
    if (model->action(index) != Actions::Rebuild && model->action(index) != Actions::Replace) {
        msg(UString("buildFile: unexpected action " + actionTypeToUString(model->action(index))), index);
        return U_NOT_IMPLEMENTED;
    }

    UByteArray body;
    if (model->action(index) == Actions::Rebuild && model->rowCount(index)) {
        bool rawFile = (model->subtype(index) == EFI_FV_FILETYPE_RAW || model->subtype(index) == EFI_FV_FILETYPE_ALL);
        std::vector<UINT32> offsets;
        USTATUS result = buildChildren(index, body, rawFile ? 0 : 4, &offsets);
        if (result)
            return result;

        // PEI-phase XIP files: refuse if any PE/TE/PIC section would move, as it would need rebase
        if (isXipPeiFile(index) && !rawFile) {
            for (int i = 0; i < model->rowCount(index) && i < (int)offsets.size(); i++) {
                UModelIndex child = index.model()->index(i, 0, index);
                if (model->type(child) == Types::Section
                    && (model->subtype(child) == EFI_SECTION_PE32 || model->subtype(child) == EFI_SECTION_TE || model->subtype(child) == EFI_SECTION_PIC)
                    && model->offset(child) != model->headerSize(index) + offsets[i]) {
                    msg(UString("buildFile: an executable section of a PEI-phase file in an uncompressed volume would move, which needs PE/TE rebase (not implemented)"), child);
                    return U_REBASE_REQUIRED;
                }
            }
        }
    }
    else {
        body = model->body(index);
    }
    return buildFileWithBody(index, body, file);
}

USTATUS FfsBuilder::buildFileWithBody(const UModelIndex & index, const UByteArray & body, UByteArray & file)
{
    UByteArray header = model->header(index);
    if ((UINT32)header.size() < sizeof(EFI_FFS_FILE_HEADER))
        return U_INVALID_FILE;
    EFI_FFS_FILE_HEADER* fh = (EFI_FFS_FILE_HEADER*)header.data();

    // Volume parameters
    UINT8 volumeRevision = 2;
    UINT8 ffsVersion = 2;
    UModelIndex volume = model->findParentOfType(index, Types::Volume);
    if (volume.isValid() && !model->hasEmptyParsingData(volume)) {
        UByteArray pd = model->parsingData(volume);
        const VOLUME_PARSING_DATA* vp = (const VOLUME_PARSING_DATA*)pd.constData();
        volumeRevision = vp->revision;
        ffsVersion = vp->ffsVersion;
    }

    // Tail (revision 1 volumes only)
    bool hasTail = (volumeRevision == 1 && (fh->Attributes & FFS_ATTRIB_TAIL_PRESENT));
    UINT32 tailSize = hasTail ? sizeof(UINT16) : 0;

    // File size
    UINT32 totalSize = (UINT32)header.size() + (UINT32)body.size() + tailSize;
    if (fh->Attributes & FFS_ATTRIB_LARGE_FILE) {
        if (ffsVersion == 3 && (UINT32)header.size() >= sizeof(EFI_FFS_FILE_HEADER2)) {
            uint32ToUint24(0, fh->Size);
            ((EFI_FFS_FILE_HEADER2*)header.data())->ExtendedSize = totalSize;
        }
        else if (ffsVersion == 2 && volumeRevision == 2 && (UINT32)header.size() >= sizeof(EFI_FFS_FILE_HEADER2_LENOVO)) {
            uint32ToUint24(0, fh->Size);
            ((EFI_FFS_FILE_HEADER2_LENOVO*)header.data())->ExtendedSize = totalSize;
        }
        else {
            msg(UString("buildFile: unsupported large file header layout"), index);
            return U_EDIT_NOT_SUPPORTED;
        }
    }
    else {
        if (totalSize > 0xFFFFFF) {
            msg(UString("buildFile: resulting file size is too big for a non-large FFS file"), index);
            return U_EDIT_NOT_SUPPORTED;
        }
        uint32ToUint24(totalSize, fh->Size);
    }

    fixFileChecksums(header, body, volumeRevision);

    file = header + body;
    if (hasTail) {
        fh = (EFI_FFS_FILE_HEADER*)header.data();
        UINT8 ht = (UINT8)~fh->IntegrityCheck.Checksum.Header;
        UINT8 ft = (UINT8)~fh->IntegrityCheck.Checksum.File;
        file += (char)ht;
        file += (char)ft;
    }
    return U_SUCCESS;
}

USTATUS FfsBuilder::buildSection(const UModelIndex & index, UByteArray & section)
{
    if (!index.isValid())
        return U_INVALID_PARAMETER;

    const UINT8 action = model->action(index);
    if (action == Actions::NoAction) {
        section = model->full(index);
        return U_SUCCESS;
    }
    if (action == Actions::Remove) {
        section.clear();
        return U_SUCCESS;
    }
    if (action != Actions::Rebuild && action != Actions::Replace) {
        msg(UString("buildSection: unexpected action " + actionTypeToUString(action)), index);
        return U_NOT_IMPLEMENTED;
    }

    UByteArray header = model->header(index);
    if ((UINT32)header.size() < sizeof(EFI_COMMON_SECTION_HEADER))
        return U_INVALID_SECTION;
    const UINT8 type = ((const EFI_COMMON_SECTION_HEADER*)header.constData())->Type;

    // PE/TE/PIC images of PEI-phase XIP files would need rebase when touched
    if (type == EFI_SECTION_PE32 || type == EFI_SECTION_TE || type == EFI_SECTION_PIC) {
        UModelIndex file = model->findParentOfType(index, Types::File);
        if (action == Actions::Replace && file.isValid() && isXipPeiFile(file) && !model->compressed(index)) {
            msg(UString("buildSection: modifying an executable image of a PEI-phase file in an uncompressed volume needs PE/TE rebase, which is not implemented"), index);
            return U_REBASE_REQUIRED;
        }
    }

    UByteArray body;
    if (action == Actions::Replace || model->rowCount(index) == 0) {
        body = model->body(index);
    }
    else {
        UByteArray children;
        USTATUS result = buildChildren(index, children, isLeafOnlyRawContainer(type) ? 0 : 4, NULL);
        if (result)
            return result;

        const bool isCompressionSection = (type == EFI_SECTION_COMPRESSION);
        const bool isGuided = (type == EFI_SECTION_GUID_DEFINED);

        if (isCompressionSection) {
            UINT8 algorithm = COMPRESSION_ALGORITHM_NONE, compressionType = EFI_NOT_COMPRESSED;
            UINT32 dictionarySize = 0;
            if (!model->hasEmptyParsingData(index)) {
                UByteArray pd = model->parsingData(index);
                const COMPRESSED_SECTION_PARSING_DATA* cp = (const COMPRESSED_SECTION_PARSING_DATA*)pd.constData();
                algorithm = cp->algorithm; compressionType = cp->compressionType; dictionarySize = cp->dictionarySize;
            }
            if (!model->hasEmptyUncompressedData(index) && children == model->uncompressedData(index)) {
                body = model->body(index); // nothing changed inside, keep the original compressed stream
            }
            else if (compressionType == EFI_NOT_COMPRESSED || algorithm == COMPRESSION_ALGORITHM_NONE) {
                body = children;
            }
            else {
                result = compressData(children, algorithm, dictionarySize, body);
                if (result) {
                    msg(UString("buildSection: compression failed (algorithm unsupported or compressor error): ") + errorCodeToUString(result), index);
                    return result;
                }
            }
            // Update UncompressedLength in the compression section header
            UINT32 off = commonSectionHeaderSize(header);
            if ((UINT32)header.size() < off + sizeof(EFI_COMPRESSION_SECTION))
                return U_INVALID_SECTION;
            ((EFI_COMPRESSION_SECTION*)(header.data() + off))->UncompressedLength = (UINT32)children.size();
        }
        else if (isGuided) {
            UINT32 off = commonSectionHeaderSize(header);
            if ((UINT32)header.size() < off + sizeof(EFI_GUID_DEFINED_SECTION))
                return U_INVALID_SECTION;
            UByteArray guid(header.constData() + off, sizeof(EFI_GUID));
            if (model->compressed(index) && !model->hasEmptyUncompressedData(index) && children == model->uncompressedData(index)) {
                body = model->body(index);
            }
            else if (guid == EFI_GUIDED_SECTION_LZMA || guid == EFI_GUIDED_SECTION_LZMA_HP || guid == EFI_GUIDED_SECTION_LZMA_MS
                     || guid == EFI_GUIDED_SECTION_LZMAF86) {
                // LZMA properties header carries the dictionary size in bytes 1..4 of the original stream
                UByteArray orig = model->body(index);
                UINT32 dictionarySize = DEFAULT_LZMA_DICTIONARY_SIZE;
                if (orig.size() >= 5)
                    memcpy(&dictionarySize, orig.constData() + 1, sizeof(UINT32));
                result = compressData(children, guid == EFI_GUIDED_SECTION_LZMAF86 ? COMPRESSION_ALGORITHM_LZMAF86 : COMPRESSION_ALGORITHM_LZMA, dictionarySize, body);
                if (result) {
                    msg(UString("buildSection: LZMA compression failed: ") + errorCodeToUString(result), index);
                    return result;
                }
            }
            else if (guid == EFI_GUIDED_SECTION_CRC32 || !model->compressed(index)) {
                body = children; // CRC32 is recomputed below, other non-compressing GUIDed sections are kept as is
                if (guid != EFI_GUIDED_SECTION_CRC32)
                    msg(UString("buildSection: contents of a GUID-defined section changed, authentication info may become invalid"), index);
            }
            else {
                msg(UString("buildSection: rebuilding of this compressed GUID-defined section (Tiano/GZip/Zlib/Brotli) after a change is not implemented"), index);
                return U_EDIT_NOT_SUPPORTED;
            }
        }
        else {
            body = children;
        }
    }

    UINT32 totalSize = (UINT32)header.size() + (UINT32)body.size();
    USTATUS res = setSectionSize(header, totalSize);
    if (res) {
        msg(UString("buildSection: section is too big for its header type"), index);
        return res;
    }

    // CRC32 GUID-defined section: recompute the CRC32 stored right after the GUID-defined header
    if (type == EFI_SECTION_GUID_DEFINED) {
        UINT32 off = commonSectionHeaderSize(header);
        if ((UINT32)header.size() >= off + sizeof(EFI_GUID_DEFINED_SECTION)) {
            UByteArray guid(header.constData() + off, sizeof(EFI_GUID));
            if (guid == EFI_GUIDED_SECTION_CRC32 && (UINT32)header.size() >= off + sizeof(EFI_GUID_DEFINED_SECTION) + sizeof(UINT32)) {
                UINT16 dataOffset = ((const EFI_GUID_DEFINED_SECTION*)(header.constData() + off))->DataOffset;
                UByteArray whole = header + body;
                if (dataOffset <= whole.size()) {
                    UINT32 crc = (UINT32)crc32(0, (const UINT8*)whole.constData() + dataOffset, (uInt)(whole.size() - dataOffset));
                    memcpy(header.data() + off + sizeof(EFI_GUID_DEFINED_SECTION), &crc, sizeof(UINT32));
                }
            }
        }
    }

    section = header + body + model->tail(index);
    return U_SUCCESS;
}

// Builds the body of an FFS volume keeping its total size unchanged.
USTATUS FfsBuilder::buildFfsVolumeBody(const UModelIndex & index, UByteArray & newBody)
{
    UByteArray pd = model->parsingData(index);
    VOLUME_PARSING_DATA vp = {};
    if (pd.size() >= (int)sizeof(VOLUME_PARSING_DATA))
        memcpy(&vp, pd.constData(), sizeof(VOLUME_PARSING_DATA));
    const UINT8 emptyByte = vp.emptyByte;
    const UINT32 origBodySize = model->bodySize(index);

    enum { SEG_FILE, SEG_PAD, SEG_FREE, SEG_OTHER };
    struct Seg {
        int kind;
        UModelIndex idx;
        UByteArray bytes;
        UINT32 origSize;
        UINT32 newSize;
        bool regen;      // pad/free span must be regenerated with newSize
        bool padMovable; // pad file only contains empty bytes
        UByteArray padHeader;
    };
    std::vector<Seg> segs;
    UByteArray padTemplate;

    for (int i = 0; i < model->rowCount(index); i++) {
        UModelIndex child = index.model()->index(i, 0, index);
        Seg s;
        s.kind = SEG_OTHER; s.idx = child; s.regen = false; s.padMovable = false;
        UINT32 alignLen = model->alignmentBytes(child).size();
        s.origSize = model->fullSize(child) + alignLen;
        UByteArray data;
        USTATUS result = buildChild(child, data);
        if (result) {
            msg(UString("buildVolume: building of ") + model->name(child) + UString(" failed with error ") + errorCodeToUString(result), child);
            return result;
        }
        if (model->type(child) == Types::File) {
            if (model->subtype(child) == EFI_FV_FILETYPE_PAD) {
                s.kind = SEG_PAD;
                s.padHeader = model->header(child);
                if (padTemplate.isEmpty())
                    padTemplate = s.padHeader;
                s.padMovable = isUniformByte(model->body(child), emptyByte) != 0 || model->body(child).isEmpty();
                s.bytes = data + model->alignmentBytes(child);
            }
            else {
                s.kind = SEG_FILE;
                if (data.size() == (int)model->fullSize(child))
                    s.bytes = data + model->alignmentBytes(child);
                else if (data.isEmpty())
                    s.bytes = data;
                else
                    s.bytes = data + UByteArray((size_t)(ALIGN8((UINT32)data.size()) - (UINT32)data.size()), (char)emptyByte);
            }
        }
        else if (model->type(child) == Types::FreeSpace) {
            s.kind = SEG_FREE;
            s.bytes = data;
        }
        else {
            s.kind = SEG_OTHER;
            s.bytes = data + model->alignmentBytes(child);
        }
        s.newSize = (UINT32)s.bytes.size();
        segs.push_back(s);
    }

    const UINT8 revision = vp.revision ? vp.revision : 2;
    auto canShiftFile = [&](const UModelIndex & f, const UINT8 rev) -> bool {
        const EFI_FFS_FILE_HEADER* fh = (const EFI_FFS_FILE_HEADER*)model->header(f).constData();
        if (isXipPeiFile(f) || model->fixed(f) || (fh->Attributes & FFS_ATTRIB_FIXED))
            return false;
        if ((fh->Attributes & FFS_ATTRIB_DATA_ALIGNMENT) != 0)
            return false;
        if (rev > 1 && (fh->Attributes & FFS_ATTRIB_DATA_ALIGNMENT2))
            return false;
        if (model->header(f).left(sizeof(EFI_GUID)) == EFI_FFS_VOLUME_TOP_FILE_GUID)
            return false;
        return true;
    };

    // Absorb size changes into following pad files / free space; files move only when canShiftFile() allows it
    for (size_t i = 0; i < segs.size(); i++) {
        if (segs[i].kind == SEG_PAD || segs[i].kind == SEG_FREE)
            continue;
        if (segs[i].newSize == segs[i].origSize)
            continue;
        if (segs[i].newSize < segs[i].origSize) {
            UINT32 freed = segs[i].origSize - segs[i].newSize;
            if (i + 1 < segs.size() && (segs[i + 1].kind == SEG_FREE || (segs[i + 1].kind == SEG_PAD && segs[i + 1].padMovable))) {
                segs[i + 1].newSize += freed;
                segs[i + 1].regen = true;
            }
            else if (freed >= sizeof(EFI_FFS_FILE_HEADER)) {
                Seg pad;
                pad.kind = SEG_PAD; pad.regen = true; pad.padMovable = true; pad.origSize = 0; pad.newSize = freed;
                pad.padHeader = padTemplate;
                segs.insert(segs.begin() + i + 1, pad);
            }
            else {
                // Too small for a pad file: pull following shiftable files back until free space / a pad file is reached
                size_t j = i + 1;
                while (j < segs.size() && segs[j].kind == SEG_FILE && canShiftFile(segs[j].idx, revision))
                    j++;
                if (j < segs.size() && (segs[j].kind == SEG_FREE || (segs[j].kind == SEG_PAD && segs[j].padMovable))) {
                    segs[j].newSize += freed;
                    segs[j].regen = true;
                }
                else {
                    msg(usprintf("buildVolume: item shrinks by %u bytes, which is too small for a pad file, and no free space is reachable by shifting files", freed), segs[i].idx);
                    return U_VOLUME_FULL;
                }
            }
        }
        else {
            UINT32 need = segs[i].newSize - segs[i].origSize;
            size_t j = i + 1;
            while (need > 0) {
                // Files directly behind the edited one may be shifted towards the end of the volume to reach free space,
                // as long as moving them is harmless: not XIP/PEI-phase, not fixed, not the VTF, no alignment above 8 bytes
                if (j < segs.size() && segs[j].kind == SEG_FILE && canShiftFile(segs[j].idx, revision)) {
                    j++;
                    continue;
                }
                if (j >= segs.size() || !(segs[j].kind == SEG_FREE || (segs[j].kind == SEG_PAD && segs[j].padMovable))) {
                    msg(UString("buildVolume: new content of ") + model->name(segs[i].idx)
                        + usprintf(" needs %u more bytes than are available (shiftable files followed by free space/pad file); "
                                   "volume growth and moving of fixed, aligned or PEI-phase files are not supported", need), segs[i].idx);
                    return U_VOLUME_FULL;
                }
                if (segs[j].newSize <= need) {
                    need -= segs[j].newSize;
                    segs[j].newSize = 0;
                    segs[j].regen = true;
                    j++;
                }
                else {
                    UINT32 left = segs[j].newSize - need;
                    if (segs[j].kind == SEG_PAD && left < sizeof(EFI_FFS_FILE_HEADER)) {
                        msg(usprintf("buildVolume: only %u bytes would remain in the pad file after the edit, too small for a pad file", left), segs[j].idx);
                        return U_VOLUME_FULL;
                    }
                    segs[j].newSize = left;
                    segs[j].regen = true;
                    need = 0;
                }
            }
        }
    }

    // Used-space fields / Apple CRC32 would need updating when anything moved or changed
    bool changed = false;
    for (size_t i = 0; i < segs.size(); i++)
        if (segs[i].regen || (segs[i].kind != SEG_PAD && segs[i].kind != SEG_FREE && segs[i].bytes != model->full(segs[i].idx) + model->alignmentBytes(segs[i].idx)))
            changed = true;
    if (changed && vp.hasAppleCrc32) {
        msg(UString("buildVolume: Apple CRC32 volumes are not supported for editing yet"), index);
        return U_EDIT_NOT_SUPPORTED;
    }

    // Materialize
    newBody.clear();
    for (size_t i = 0; i < segs.size(); i++) {
        Seg & s = segs[i];
        if (s.kind == SEG_FREE) {
            if (s.regen) {
                if (s.newSize == 0) continue;
                newBody += UByteArray((size_t)s.newSize, (char)emptyByte);
            }
            else newBody += s.bytes;
        }
        else if (s.kind == SEG_PAD && s.regen) {
            if (s.newSize == 0) continue;
            if (s.newSize < sizeof(EFI_FFS_FILE_HEADER)) {
                msg(UString("buildVolume: pad file would become smaller than its header"), index);
                return U_VOLUME_FULL;
            }
            UByteArray hdr = s.padHeader;
            if (hdr.size() != (int)sizeof(EFI_FFS_FILE_HEADER)) {
                hdr = UByteArray(sizeof(EFI_FFS_FILE_HEADER), (char)emptyByte);
                EFI_FFS_FILE_HEADER* nh = (EFI_FFS_FILE_HEADER*)hdr.data();
                nh->Attributes = 0;
                nh->Type = EFI_FV_FILETYPE_PAD;
                UINT8 state = EFI_FILE_DATA_VALID | EFI_FILE_HEADER_VALID | EFI_FILE_HEADER_CONSTRUCTION;
                nh->State = emptyByte ? (UINT8)~state : state;
            }
            uint32ToUint24(s.newSize, ((EFI_FFS_FILE_HEADER*)hdr.data())->Size);
            UByteArray body((size_t)(s.newSize - sizeof(EFI_FFS_FILE_HEADER)), (char)emptyByte);
            fixFileChecksums(hdr, body, revision);
            newBody += hdr;
            newBody += body;
        }
        else {
            newBody += s.bytes;
        }
    }

    if ((UINT32)newBody.size() != origBodySize) {
        msg(usprintf("buildVolume: internal error, rebuilt volume body is %u bytes, expected %u", (UINT32)newBody.size(), origBodySize), index);
        return U_INVALID_VOLUME;
    }

    // If the start of free space moved and the header has a valid used-space value, it would go stale
    if (vp.hasValidUsedSpace) {
        UINT32 oldOff = 0, newOff = 0; bool seenOld = false, seenNew = false;
        UINT32 pos = 0;
        for (size_t i = 0; i < segs.size(); i++) {
            if (segs[i].kind == SEG_FREE && !seenNew && segs[i].newSize) { newOff = pos; seenNew = true; }
            pos += segs[i].newSize;
        }
        pos = 0;
        for (size_t i = 0; i < segs.size(); i++) {
            if (segs[i].kind == SEG_FREE && !seenOld && segs[i].origSize) { oldOff = pos; seenOld = true; }
            pos += segs[i].origSize;
        }
        if (seenOld != seenNew || oldOff != newOff) {
            msg(UString("buildVolume: volume has a valid UsedSpace field that would need updating, not supported"), index);
            return U_EDIT_NOT_SUPPORTED;
        }
    }
    return U_SUCCESS;
}

USTATUS FfsBuilder::buildVolume(const UModelIndex & index, UByteArray & volume)
{
    if (!index.isValid())
        return U_INVALID_PARAMETER;

    if (model->action(index) == Actions::NoAction) {
        volume = model->full(index);
        return U_SUCCESS;
    }
    if (model->action(index) == Actions::Remove) {
        volume.clear();
        return U_SUCCESS;
    }
    if (model->action(index) == Actions::Replace) {
        volume = model->header(index) + model->body(index) + model->tail(index);
        if ((UINT32)volume.size() != model->fullSize(index) && false)
            return U_INVALID_VOLUME;
        return U_SUCCESS;
    }
    if (model->action(index) != Actions::Rebuild) {
        msg(UString("buildVolume: unexpected action " + actionTypeToUString(model->action(index))), index);
        return U_NOT_IMPLEMENTED;
    }

    // Non-FFS volumes (NVRAM, Apple microcode, unknown) can only be passed through unchanged
    UINT8 subtype = model->subtype(index);
    if (subtype != Subtypes::Ffs2Volume && subtype != Subtypes::Ffs3Volume) {
        bool childChanged = false;
        for (int i = 0; i < model->rowCount(index); i++)
            if (isModified(index.model()->index(i, 0, index)))
                childChanged = true;
        if (childChanged) {
            msg(UString("buildVolume: rebuilding volumes that are not FFS volumes is not supported"), index);
            return U_EDIT_NOT_SUPPORTED;
        }
        volume = model->full(index);
        return U_SUCCESS;
    }

    UByteArray header = model->header(index);
    if ((UINT32)header.size() < sizeof(EFI_FIRMWARE_VOLUME_HEADER))
        return U_INVALID_VOLUME;

    UByteArray body;
    USTATUS result = buildFfsVolumeBody(index, body);
    if (result)
        return result;

    // Header checksum is recomputed (header itself is not changed: volumes keep their size)
    EFI_FIRMWARE_VOLUME_HEADER* vh = (EFI_FIRMWARE_VOLUME_HEADER*)header.data();
    vh->Checksum = 0;
    vh->Checksum = calculateChecksum16((const UINT16*)header.constData(), vh->HeaderLength);

    volume = header + body + model->tail(index);
    return U_SUCCESS;
}
