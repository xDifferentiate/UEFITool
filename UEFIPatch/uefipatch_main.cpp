/* uefipatch_main.cpp

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
#include <sstream>
#include <cstring>

#include "../version.h"
#include "../common/filesystem.h"
#include "../common/utility.h"
#include "uefipatch.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cout << "UEFIPatch " PROGRAM_VERSION " - UEFI image file patching utility (NE, same-size patches only)" << std::endl << std::endl
            << "Usage: UEFIPatch image_file [patches.txt] [-o output]" << std::endl
            << "Usage: UEFIPatch image_file [-p \"Guid SectionType Patch\"] [-o output]" << std::endl << std::endl
            << "Patches will be read from patches.txt file by default" << std::endl;
        return 0;
    }

    std::string inputPath = argv[1];
    std::string patches = "patches.txt";
    std::string outputPath = inputPath + ".patched";
    bool fromArg = false, patchesGiven = false;
    for (int i = 2; i < argc; i++) {
        if ((!std::strcmp(argv[i], "-p") || !std::strcmp(argv[i], "--patch")) && i + 1 < argc) {
            fromArg = true;
            patches = argv[++i];
        }
        else if ((!std::strcmp(argv[i], "-o") || !std::strcmp(argv[i], "--output")) && i + 1 < argc) {
            outputPath = argv[++i];
        }
        else if (!patchesGiven && !fromArg) {
            patches = argv[i];
            patchesGiven = true;
        }
        else {
            std::cout << "Function called with invalid parameter" << std::endl;
            return U_INVALID_PARAMETER;
        }
    }

    std::string patchText;
    if (fromArg) {
        patchText = patches;
    }
    else {
        std::ifstream pf(patches.c_str(), std::ios::binary);
        if (!pf) {
            std::cout << patches << " file not found or can't be read" << std::endl;
            return U_INVALID_FILE;
        }
        std::stringstream ss; ss << pf.rdbuf();
        patchText = ss.str();
    }

    UByteArray image;
    if (!readFileIntoBuffer(getAbsPath(inputPath.c_str()), image)) {
        std::cout << "Input file not found or can't be read" << std::endl;
        return U_FILE_OPEN;
    }

    UEFIPatch patcher;
    UByteArray rebuilt;
    std::vector<UString> messages;
    USTATUS result = patcher.patch(image, patchText, rebuilt, messages);
    for (size_t i = 0; i < messages.size(); i++)
        std::cerr << messages[i].toLocal8Bit() << std::endl;

    switch (result) {
    case U_SUCCESS: {
        std::ofstream out(outputPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cout << "Output file can't be written" << std::endl;
            return U_FILE_WRITE;
        }
        out.write(rebuilt.constData(), rebuilt.size());
        out.close();
        std::cout << "Image patched" << std::endl;
        break;
    }
    case U_INVALID_PARAMETER:           std::cout << "Function called with invalid parameter" << std::endl; break;
    case U_NOTHING_TO_PATCH:            std::cout << "No patches can be applied to input file" << std::endl; break;
    case U_UNKNOWN_PATCH_TYPE:          std::cout << "Unknown patch type" << std::endl; break;
    case U_PATCH_OFFSET_OUT_OF_BOUNDS:  std::cout << "Patch offset out of bounds" << std::endl; break;
    case U_INVALID_SYMBOL:              std::cout << "Pattern format mismatch" << std::endl; break;
    default:
        std::cout << "Error " << (int)result << ": " << errorCodeToUString(result).toLocal8Bit() << std::endl;
    }
    return (int)result;
}
