/* fssbuilder.h

Copyright (c) 2016, Nikolaj Schlej. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

*/

#ifndef FFSBUILDER_H
#define FFSBUILDER_H

#include <vector>

#include "basetypes.h"
#include "ubytearray.h"
#include "ustring.h"
#include "treemodel.h"

class FfsBuilder
{
public:
    FfsBuilder(const TreeModel * treeModel) : model(treeModel) {}
    ~FfsBuilder() {}

    std::vector<std::pair<UString, UModelIndex> > getMessages() const { return messagesVector; }
    void clearMessages() { messagesVector.clear(); }

    USTATUS build(const UModelIndex & root, UByteArray & image);

private:
    const TreeModel* model;
    std::vector<std::pair<UString, UModelIndex> > messagesVector;
    void msg(const UString & message, const UModelIndex &index = UModelIndex()) {
        messagesVector.push_back(std::pair<UString, UModelIndex>(message, index));
    }

    USTATUS buildCapsule(const UModelIndex & index, UByteArray & capsule);
    USTATUS buildIntelImage(const UModelIndex & index, UByteArray & intelImage);
    USTATUS buildRawArea(const UModelIndex & index, UByteArray & rawArea);
    USTATUS buildPadding(const UModelIndex & index, UByteArray & padding);
    USTATUS buildVolume(const UModelIndex & index, UByteArray & volume);
    USTATUS buildNonUefiData(const UModelIndex & index, UByteArray & data);
    USTATUS buildFreeSpace(const UModelIndex & index, UByteArray & freeSpace);
    USTATUS buildPadFile(const UModelIndex & index, UByteArray & padFile);
    USTATUS buildFile(const UModelIndex & index, UByteArray & file);
    USTATUS buildSection(const UModelIndex & index, UByteArray & section);
    
    // Generic dispatcher for any child item
    USTATUS buildChild(const UModelIndex & index, UByteArray & built);
    // Concatenates built children of a container item (sections, raw area items), preserving alignment bytes
    USTATUS buildChildren(const UModelIndex & index, UByteArray & built, const UINT32 sectionAlignment, std::vector<UINT32> * offsets);
    USTATUS buildFfsVolumeBody(const UModelIndex & index, UByteArray & body);
    USTATUS buildFileWithBody(const UModelIndex & index, const UByteArray & body, UByteArray & file);
    // Returns true if the item or any of its descendants has an action other than NoAction
    bool isModified(const UModelIndex & index) const;
    bool isXipPeiFile(const UModelIndex & fileIndex) const;

    // Utility functions
    USTATUS erase(const UModelIndex & index, UByteArray & erased);
};

#endif // FFSBUILDER_H
