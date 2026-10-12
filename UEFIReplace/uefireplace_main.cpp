/* uefireplace_main.cpp

Copyright (c) 2026, UEFITool contributors. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>

#include "../version.h"
#include "../common/filesystem.h"
#include "../common/ffs.h"
#include "../common/utility.h"
#include "uefireplace.h"

static void print_usage()
{
    std::cout << "UEFIReplace " PROGRAM_VERSION " - UEFI image file replacement utility (NE, size-preserving)" << std::endl << std::endl
        << "Usage: UEFIReplace image_file guid section_type contents_file [-o output] [-all] [-asis]" << std::endl
        << "  guid          GUID of the FFS file (or subtype GUID of a freeform subtype GUID section)" << std::endl
        << "  section_type  hex item type to replace (section type; file type when replacing whole files)" << std::endl
        << "  -o output     output file, default is image_file.patched" << std::endl
        << "  -all          replace all matches, default is the first only" << std::endl
        << "  -asis         contents include the item header, default is body only" << std::endl << std::endl
        << "The image size never changes: edits needing volume growth or PE/TE rebase are refused." << std::endl;
}

int main(int argc, char *argv[])
{
    if (argc < 5) {
        print_usage();
        return 0;
    }

    EFI_GUID efiGuid;
    if (!ustringToGuid(UString(argv[2]), efiGuid)) {
        std::cerr << "Invalid GUID, expected XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" << std::endl;
        return U_INVALID_PARAMETER;
    }
    UByteArray guid((const char*)&efiGuid, sizeof(EFI_GUID));

    char* end = NULL;
    unsigned long st = std::strtoul(argv[3], &end, 16);
    if (!end || *end != 0 || st > 0xFF) {
        std::cerr << "Invalid section type, expected hex byte" << std::endl;
        return U_INVALID_PARAMETER;
    }
    UINT8 sectionType = (UINT8)st;

    std::string output = std::string(argv[1]) + ".patched";
    bool replaceOnce = true;
    bool asIs = false;
    for (int i = 5; i < argc; i++) {
        if ((!std::strcmp(argv[i], "-o") || !std::strcmp(argv[i], "--output")) && i + 1 < argc) {
            output = argv[++i];
        }
        else if (!std::strcmp(argv[i], "-all")) {
            replaceOnce = false;
        }
        else if (!std::strcmp(argv[i], "-asis")) {
            asIs = true;
        }
        else {
            std::cerr << "Unknown argument " << argv[i] << std::endl;
            return U_INVALID_PARAMETER;
        }
    }

    UByteArray image, contents;
    if (!readFileIntoBuffer(getAbsPath(argv[1]), image)) {
        std::cerr << "Input file not found or can't be read" << std::endl;
        return U_FILE_OPEN;
    }
    if (!readFileIntoBuffer(getAbsPath(argv[4]), contents)) {
        std::cerr << "Contents file not found or can't be read" << std::endl;
        return U_FILE_OPEN;
    }

    UEFIReplace replacer;
    UByteArray rebuilt;
    std::vector<UString> messages;
    USTATUS result = replacer.replace(image, guid, sectionType, contents, asIs, replaceOnce, rebuilt, messages);
    for (size_t i = 0; i < messages.size(); i++)
        std::cerr << messages[i].toLocal8Bit() << std::endl;

    if (result == U_SUCCESS) {
        std::ofstream out(output.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "Output file can't be written" << std::endl;
            return U_FILE_WRITE;
        }
        out.write(rebuilt.constData(), rebuilt.size());
        out.close();
        std::cout << "File replaced" << std::endl;
        return 0;
    }
    if (result == U_NOTHING_TO_PATCH)
        std::cerr << "No replacements can be applied to the input file (no match, or result identical to input)" << std::endl;
    else
        std::cerr << "Replace failed: " << errorCodeToUString(result).toLocal8Bit() << " (" << (int)result << ")" << std::endl;
    return (int)result;
}
