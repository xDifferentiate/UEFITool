/* uefipatch.cpp

Copyright (c) 2026, UEFITool contributors. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

#include <cstdio>
#include <cstdlib>
#include <cctype>

#include "uefipatch.h"
#include "../common/ffs.h"
#include "../common/utility.h"

static int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string toHexUpper(const UByteArray & d, int pos, int len)
{
    std::string s;
    char buf[3];
    for (int i = pos; i < pos + len; i++) {
        snprintf(buf, sizeof(buf), "%02X", (unsigned)(UINT8)d.at(i));
        s += buf;
    }
    return s;
}

USTATUS uefiPatchViaOffset(UByteArray & data, const UINT32 offset, const std::string & hexReplacePattern, std::vector<UString> * messages)
{
    // Skip patterns with odd length
    if (hexReplacePattern.size() % 2 > 0)
        return U_INVALID_PARAMETER;

    const UINT32 count = (UINT32)(hexReplacePattern.size() / 2);
    // Check offset bounds (old_engine did this in UINT32 and could wrap when the pattern is longer than the body)
    if ((UINT64)offset + count > (UINT64)data.size())
        return U_PATCH_OFFSET_OUT_OF_BOUNDS;

    UByteArray replacePattern;
    for (UINT32 i = 0; i < count; i++) {
        const char hi = hexReplacePattern[2 * i], lo = hexReplacePattern[2 * i + 1];
        const UINT8 old = (UINT8)data.at(offset + i);
        UINT8 value;
        if (hi == '.' && lo == '.') {           // full byte placeholder
            value = old;
        }
        else if (hi == '.') {                    // upper nibble placeholder
            int l = hexValue(lo);
            if (l < 0) return U_INVALID_SYMBOL;
            value = (UINT8)((old & 0xF0) + l);
        }
        else if (lo == '.') {                    // lower nibble placeholder
            int h = hexValue(hi);
            if (h < 0) return U_INVALID_SYMBOL;
            value = (UINT8)((old & 0x0F) + (h << 4));
        }
        else {
            int h = hexValue(hi), l = hexValue(lo);
            if (h < 0 || l < 0) return U_INVALID_SYMBOL;
            value = (UINT8)((h << 4) | l);
        }
        replacePattern += UByteArray(1, (char)value);
    }

    if (messages) {
        char off[16];
        snprintf(off, sizeof(off), "%X", (unsigned)offset);
        messages->push_back(UString("patch: replaced ") + UString(std::to_string(count).c_str()) + UString(" bytes at offset ")
            + UString(off) + UString("h ") + UString(toHexUpper(data, (int)offset, (int)count).c_str()) + UString(" -> ")
            + UString(toHexUpper(replacePattern, 0, (int)count).c_str()));
    }
    for (UINT32 i = 0; i < count; i++)
        data[offset + i] = replacePattern.at(i);
    return U_SUCCESS;
}

USTATUS uefiPatchViaPattern(UByteArray & data, const std::string & hexFindPattern, const std::string & hexReplacePattern, std::vector<UString> * messages)
{
    // Skip patterns with odd length
    if (hexFindPattern.size() % 2 > 0 || hexReplacePattern.size() % 2 > 0)
        return U_INVALID_PARAMETER;
    // old_engine used the find pattern as a QRegExp; patches.txt only uses hex digits and '.', which is all we accept
    if (hexFindPattern.empty())
        return U_INVALID_PARAMETER;
    for (size_t i = 0; i < hexFindPattern.size(); i++)
        if (hexFindPattern[i] != '.' && hexValue(hexFindPattern[i]) < 0)
            return U_INVALID_SYMBOL;

    // Matching runs over the hex string of the ORIGINAL body (as old_engine); replacements go into a working copy
    std::string hexBody = toHexUpper(data, 0, data.size());
    UByteArray body = data;
    const size_t n = hexFindPattern.size();
    std::vector<int> pat(n);
    for (size_t i = 0; i < n; i++)
        pat[i] = hexFindPattern[i] == '.' ? -1 : hexValue(hexFindPattern[i]);

    size_t start = 0;
    while (hexBody.size() >= n && start <= hexBody.size() - n) {
        // first match at or after start (QRegExp::indexIn semantics)
        size_t found = std::string::npos;
        for (size_t off = start; off + n <= hexBody.size(); off++) {
            size_t k = 0;
            for (; k < n; k++)
                if (pat[k] >= 0 && hexValue(hexBody[off + k]) != pat[k])
                    break;
            if (k == n) { found = off; break; }
        }
        if (found == std::string::npos)
            break;
        // Only matches on byte boundaries are used
        if (found % 2 == 0) {
            USTATUS result = uefiPatchViaOffset(body, (UINT32)(found / 2), hexReplacePattern, messages);
            if (result)
                return result;
        }
        start = found + 1;
    }

    data = body;
    return U_SUCCESS;
}

USTATUS UEFIPatch::patchSection(TreeModel * model, FfsOperations * ops, const UModelIndex & index,
                                const std::vector<UefiPatchData> & patches, std::vector<UString> & messages)
{
    // same preconditions as old_engine's FfsEngine::patch
    if (!index.isValid() || patches.empty() || model->rowCount(index))
        return U_INVALID_PARAMETER;
    if (model->action(index) == Actions::Remove)
        return U_NOTHING_TO_PATCH;

    UByteArray body = model->body(index);
    for (size_t i = 0; i < patches.size(); i++) {
        USTATUS result;
        if (patches[i].type == PATCH_TYPE_OFFSET)
            result = uefiPatchViaOffset(body, patches[i].offset, patches[i].hexReplacePattern, &messages);
        else if (patches[i].type == PATCH_TYPE_PATTERN)
            result = uefiPatchViaPattern(body, patches[i].hexFindPattern, patches[i].hexReplacePattern, &messages);
        else
            return U_UNKNOWN_PATCH_TYPE;
        if (result)
            return result;
    }

    if (body != model->body(index)) {
        UByteArray patched = model->header(index);
        patched += body;
        // In place replace + parent chain marked for rebuild (done by FfsOperations::replace)
        return ops->replace(index, patched, REPLACE_MODE_AS_IS);
    }
    return U_NOTHING_TO_PATCH;
}

// Mirrors old_engine's UEFIPatch::patchFile, including its quirk: after a section was patched the loop over the
// siblings stops and U_NOTHING_TO_PATCH is returned to the caller, so at most one section per parent is patched per line.
USTATUS UEFIPatch::patchSections(TreeModel * model, FfsOperations * ops, const UModelIndex & index, const UByteArray & fileGuid,
                                 const UINT8 sectionType, const std::vector<UefiPatchData> & patches, std::vector<UString> & messages)
{
    if (!model || !index.isValid())
        return U_INVALID_PARAMETER;
    if (model->type(index) == Types::Section && model->subtype(index) == sectionType) {
        UModelIndex fileIndex = model->findParentOfType(index, Types::File);
        if (fileIndex.isValid() && model->type(fileIndex) == Types::File
            && model->header(fileIndex).left(sizeof(EFI_GUID)) == fileGuid)
            return patchSection(model, ops, index, patches, messages);
    }

    for (int i = 0; i < model->rowCount(index); i++) {
        USTATUS result = patchSections(model, ops, model->index(i, 0, index), fileGuid, sectionType, patches, messages);
        if (!result)
            break;
        else if (result != U_NOTHING_TO_PATCH)
            return result;
    }
    return U_NOTHING_TO_PATCH;
}

static std::string trimmed(const std::string & s)
{
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

static std::vector<std::string> splitOn(const std::string & s, char c)
{
    std::vector<std::string> r;
    size_t pos = 0;
    while (true) {
        size_t n = s.find(c, pos);
        if (n == std::string::npos) { r.push_back(s.substr(pos)); break; }
        r.push_back(s.substr(pos, n - pos));
        pos = n + 1;
    }
    return r;
}

USTATUS UEFIPatch::patch(const UByteArray & image, const std::string & patchText, UByteArray & rebuilt, std::vector<UString> & messages)
{
    rebuilt.clear();
    TreeModel model;
    FfsParser parser(&model);
    USTATUS result = parser.parse(image);
    if (result)
        return result;
    FfsOperations ops(&model);

    size_t pos = 0;
    while (pos <= patchText.size()) {
        size_t nl = patchText.find('\n', pos);
        std::string line = trimmed(patchText.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = (nl == std::string::npos) ? patchText.size() + 1 : nl + 1;
        // Sharp sign is a comment
        if (line.empty() || line[0] == '#')
            continue;

        std::vector<std::string> list = splitOn(line, ' ');
        if (list.size() < 3)
            continue;

        EFI_GUID efiGuid;
        if (!ustringToGuid(UString(list[0].c_str()), efiGuid)) {
            messages.push_back(UString("invalid GUID in patch line: ") + UString(line.c_str()));
            return U_INVALID_PARAMETER;
        }
        UByteArray guid((const char*)&efiGuid, sizeof(EFI_GUID));

        char * end = NULL;
        unsigned long st = std::strtoul(list[1].c_str(), &end, 16);
        if (list[1].empty() || !end || *end != 0 || st > 0xFF) {
            messages.push_back(UString("invalid section type in patch line: ") + UString(line.c_str()));
            return U_INVALID_PARAMETER;
        }
        const UINT8 sectionType = (UINT8)st;

        std::vector<UefiPatchData> patches;
        for (size_t i = 2; i < list.size(); i++) {
            std::vector<std::string> pl = splitOn(list[i], ':');
            UefiPatchData p;
            p.type = pl[0].empty() ? 0 : (UINT8)pl[0][0];
            p.offset = 0xFFFFFFFF;
            if (p.type == PATCH_TYPE_PATTERN) {
                if (pl.size() < 3) { messages.push_back(UString("malformed patch: ") + UString(list[i].c_str())); return U_INVALID_PARAMETER; }
                p.hexFindPattern = pl[1];
                p.hexReplacePattern = pl[2];
                patches.push_back(p);
            }
            else if (p.type == PATCH_TYPE_OFFSET) {
                if (pl.size() < 3) { messages.push_back(UString("malformed patch: ") + UString(list[i].c_str())); return U_INVALID_PARAMETER; }
                char * e2 = NULL;
                unsigned long off = std::strtoul(pl[1].c_str(), &e2, 16);
                if (pl[1].empty() || !e2 || *e2 != 0 || off > 0xFFFFFFFFul) { messages.push_back(UString("malformed offset: ") + UString(list[i].c_str())); return U_INVALID_PARAMETER; }
                p.offset = (UINT32)off;
                p.hexReplacePattern = pl[2];
                patches.push_back(p);
            }
            // Unknown patch types (and the empty entry after a trailing space) are ignored
        }

        result = patchSections(&model, &ops, model.index(0, 0), guid, sectionType, patches, messages);
        if (result && result != U_NOTHING_TO_PATCH)
            return result;
    }

    std::vector<std::pair<UString, UModelIndex> > opsMessages = ops.getMessages();
    for (size_t i = 0; i < opsMessages.size(); i++)
        messages.push_back(opsMessages[i].first);

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
    if (rebuilt == image) {
        rebuilt.clear();
        return U_NOTHING_TO_PATCH;
    }
    return U_SUCCESS;
}
