/* ffsedit_test.cpp - test harness for the size-preserving builder and UEFIReplace logic

Usage: uefireplace_test image.bin [outdir]
Writes rebuilt images to outdir (default: current directory) so they can be diffed with UEFIExtract.
Exit code is the number of failed checks.
*/
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <set>

#include "../uefireplace.h"
#include "../../common/filesystem.h"
#include "../../common/ffs.h"
#include "../../common/utility.h"

static int failures = 0;
static void check(bool ok, const std::string & name, const std::string & detail = "")
{
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!detail.empty()) std::cout << " -- " << detail;
    std::cout << std::endl;
    if (!ok) failures++;
}

static void writeFile(const std::string & path, const UByteArray & d)
{
    std::ofstream o(path.c_str(), std::ios::binary | std::ios::trunc);
    o.write(d.constData(), d.size());
}

static std::string firstDiff(const UByteArray & a, const UByteArray & b)
{
    if (a.size() != b.size())
        return "size differs: " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    long n = 0; long first = -1, last = -1;
    for (int i = 0; i < a.size(); i++)
        if (a[i] != b[i]) { if (first < 0) first = i; last = i; n++; }
    char buf[200];
    snprintf(buf, sizeof(buf), "%ld differing bytes, first at 0x%lX, last at 0x%lX", n, first, last);
    return n ? buf : "identical";
}

static void setAllRebuild(TreeModel & m, const UModelIndex & idx)
{
    UINT8 t = m.type(idx);
    bool pad = (t == Types::File && m.subtype(idx) == EFI_FV_FILETYPE_PAD);
    if (t == Types::Image || t == Types::Volume || t == Types::Section || (t == Types::File && !pad)
        || (t == Types::Region && m.subtype(idx) == Subtypes::BiosRegion) || t == Types::Capsule)
        m.setAction(idx, Actions::Rebuild);
    for (int i = 0; i < m.rowCount(idx); i++)
        setAllRebuild(m, m.index(i, 0, idx));
}

static UModelIndex findFileSection(TreeModel & m, const UModelIndex & idx, const UByteArray & guid, UINT8 secType)
{
    if (m.type(idx) == Types::Section && m.subtype(idx) == secType) {
        UModelIndex f = m.findParentOfType(idx, Types::File);
        if (f.isValid() && m.header(f).left(sizeof(EFI_GUID)) == guid)
            return idx;
    }
    for (int i = 0; i < m.rowCount(idx); i++) {
        UModelIndex r = findFileSection(m, m.index(i, 0, idx), guid, secType);
        if (r.isValid()) return r;
    }
    return UModelIndex();
}

static std::set<std::string> parseMessages(const UByteArray & image)
{
    TreeModel m; FfsParser p(&m);
    std::set<std::string> out;
    if (p.parse(image)) { out.insert("PARSE FAILED"); return out; }
    std::vector<std::pair<UString, UModelIndex> > msgs = p.getMessages();
    for (size_t i = 0; i < msgs.size(); i++)
        out.insert(std::string(msgs[i].first.toLocal8Bit()));
    return out;
}


// Collects (GUID, body) of every non-pad file except the ones expected to change (the VBT file and its parent compressed container file)
static void collectFiles(TreeModel & m, const UModelIndex & idx, std::vector<std::pair<std::string, UByteArray> > & out)
{
    if (m.type(idx) == Types::File && m.subtype(idx) != EFI_FV_FILETYPE_PAD) {
        UByteArray g = m.header(idx).left(sizeof(EFI_GUID));
        EFI_GUID guid; memcpy(&guid, g.constData(), sizeof(EFI_GUID));
        std::string name = std::string(guidToUString(guid).toLocal8Bit());
        if (name != "56752DA9-DE6B-4895-8819-1945B6B76C22" && name != "F641AC56-3B92-4154-8547-80EF6AAFF551")
            out.push_back(std::make_pair(name, m.body(idx)));
    }
    for (int i = 0; i < m.rowCount(idx); i++)
        collectFiles(m, m.index(i, 0, idx), out);
}

static std::string collateral(const UByteArray & orig, const UByteArray & rebuilt, int & nFiles)
{
    TreeModel m1, m2; FfsParser p1(&m1), p2(&m2);
    p1.parse(orig); p2.parse(rebuilt);
    std::vector<std::pair<std::string, UByteArray> > a, b;
    collectFiles(m1, m1.index(0, 0), a); collectFiles(m2, m2.index(0, 0), b);
    nFiles = (int)a.size();
    if (a.size() != b.size()) return "file count differs: " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].first != b[i].first || a[i].second != b[i].second) return "file " + a[i].first + " differs";
    return "";
}

static std::string newMessages(const std::set<std::string> & origMsgs, const UByteArray & rebuilt)
{
    std::set<std::string> nm = parseMessages(rebuilt);
    std::string r;
    for (std::set<std::string>::iterator it = nm.begin(); it != nm.end(); ++it) if (!origMsgs.count(*it)) r += *it + "; ";
    return r;
}

int main(int argc, char* argv[])
{
    if (argc < 2) { std::cout << "usage: uefireplace_test image.bin [outdir]" << std::endl; return 2; }
    std::string outdir = argc > 2 ? argv[2] : ".";
    UByteArray image;
    if (!readFileIntoBuffer(getAbsPath(argv[1]), image)) { std::cout << "can't read image" << std::endl; return 2; }
    std::cout << "image: " << argv[1] << " (" << image.size() << " bytes)" << std::endl;

    // ---- T1: identity with NoAction everywhere
    {
        TreeModel m; FfsParser p(&m);
        check(p.parse(image) == U_SUCCESS, "T1 parse");
        FfsBuilder b(&m); UByteArray out;
        USTATUS r = b.build(m.index(0, 0), out);
        check(r == U_SUCCESS && out == image, "T1 identity rebuild (NoAction)", r ? errorCodeToUString(r).toLocal8Bit() : firstDiff(out, image));
    }

    // ---- T2: forced full rebuild of every Image/Region(BIOS)/Volume/File/Section, no content change
    {
        TreeModel m; FfsParser p(&m); p.parse(image);
        setAllRebuild(m, m.index(0, 0));
        FfsBuilder b(&m); UByteArray out;
        USTATUS r = b.build(m.index(0, 0), out);
        std::string d = r ? std::string(errorCodeToUString(r).toLocal8Bit()) : firstDiff(out, image);
        check(r == U_SUCCESS && out == image, "T2 identity rebuild (everything marked Rebuild)", d);
        if (r) { std::vector<std::pair<UString, UModelIndex> > ms = b.getMessages(); for (size_t i = 0; i < ms.size() && i < 10; i++) std::cout << "    builder: " << ms[i].first.toLocal8Bit() << std::endl; }
        else if (out != image) writeFile(outdir + "/T2_rebuild.bin", out);
    }

    // Locate VBT raw section (file 56752DA9-DE6B-4895-8819-1945B6B76C22, section type 0x19)
    EFI_GUID g; ustringToGuid(UString("56752DA9-DE6B-4895-8819-1945B6B76C22"), g);
    UByteArray vbtGuid((const char*)&g, sizeof(EFI_GUID));
    UByteArray vbtBody;
    {
        TreeModel m; FfsParser p(&m); p.parse(image);
        UModelIndex s = findFileSection(m, m.index(0, 0), vbtGuid, EFI_SECTION_RAW);
        check(s.isValid(), "T3 locate VBT raw section by file GUID");
        if (s.isValid()) vbtBody = m.body(s);
        std::cout << "    VBT raw section body: " << vbtBody.size() << " bytes, starts with '" << std::string(vbtBody.constData(), 4) << "'" << std::endl;
    }
    if (vbtBody.isEmpty()) { std::cout << "FAILURES: " << failures << std::endl; return failures; }

    std::set<std::string> origMsgs = parseMessages(image);
    UEFIReplace rep;
    std::vector<UString> msgs;

    // ---- T3: same-content replace
    {
        UByteArray rebuilt, c = vbtBody; msgs.clear();
        USTATUS r = rep.replace(image, vbtGuid, EFI_SECTION_RAW, c, false, true, rebuilt, msgs);
        // identical output is reported as U_NOTHING_TO_PATCH by UEFIReplace (same as old_engine)
        check(r == U_NOTHING_TO_PATCH || (r == U_SUCCESS && rebuilt.size() == image.size()), "T3 same-content replace", r == U_NOTHING_TO_PATCH ? "output identical to input (NOTHING_TO_PATCH)" : (r ? errorCodeToUString(r).toLocal8Bit() : firstDiff(rebuilt, image)));
        for (size_t i = 0; i < msgs.size(); i++) std::cout << "    msg: " << msgs[i].toLocal8Bit() << std::endl;
    }

    // ---- T4: same-size modification, reparse and compare
    {
        UByteArray mod = vbtBody; msgs.clear();
        int pos = mod.indexOf(UByteArray("LFP_PanelName", 13));
        if (pos < 0) pos = 0x200;
        mod[pos] = 'X';
        UByteArray rebuilt, c = mod;
        USTATUS r = rep.replace(image, vbtGuid, EFI_SECTION_RAW, c, false, true, rebuilt, msgs);
        check(r == U_SUCCESS, "T4 same-size edit builds", r ? errorCodeToUString(r).toLocal8Bit() : "");
        for (size_t i = 0; i < msgs.size(); i++) std::cout << "    msg: " << msgs[i].toLocal8Bit() << std::endl;
        if (r == U_SUCCESS) {
            writeFile(outdir + "/T4_edit.bin", rebuilt);
            check(rebuilt.size() == image.size(), "T4 output size unchanged");
            TreeModel m; FfsParser p(&m);
            check(p.parse(rebuilt) == U_SUCCESS, "T4 output re-parses");
            UModelIndex s = findFileSection(m, m.index(0, 0), vbtGuid, EFI_SECTION_RAW);
            check(s.isValid() && m.body(s) == mod, "T4 VBT section in output equals modified body");
            std::set<std::string> nm = parseMessages(rebuilt);
            std::vector<std::string> added;
            for (std::set<std::string>::iterator it = nm.begin(); it != nm.end(); ++it) if (!origMsgs.count(*it)) added.push_back(*it);
            check(added.empty(), "T4 no new parser messages (checksums etc.) vs original", std::to_string(added.size()) + " new");
            int nf = 0; std::string col = collateral(image, rebuilt, nf);
            check(col.empty(), "T4 collateral: all other files (" + std::to_string(nf) + ") byte-identical", col);
            for (size_t i = 0; i < added.size() && i < 10; i++) std::cout << "    new msg: " << added[i] << std::endl;
        }
    }

    // ---- T5: smaller body (shrinks by 64 and by 8 bytes)
    {
        int shrinks[2] = { 64, 8 };
        for (int k = 0; k < 2; k++) {
            UByteArray mod = vbtBody.left(vbtBody.size() - shrinks[k]);
            UByteArray rebuilt, c = mod; msgs.clear();
            USTATUS r = rep.replace(image, vbtGuid, EFI_SECTION_RAW, c, false, true, rebuilt, msgs);
            std::string name = "T5 smaller body (-" + std::to_string(shrinks[k]) + " bytes)";
            if (r == U_SUCCESS) {
                TreeModel m; FfsParser p(&m);
                bool ok = p.parse(rebuilt) == U_SUCCESS && rebuilt.size() == image.size();
                UModelIndex s = ok ? findFileSection(m, m.index(0, 0), vbtGuid, EFI_SECTION_RAW) : UModelIndex();
                ok = ok && s.isValid() && m.body(s) == mod;
                check(ok, name + " builds and re-parses with new body");
                int nf = 0; std::string col = collateral(image, rebuilt, nf);
                check(col.empty(), name + " collateral: all other files (" + std::to_string(nf) + ") byte-identical", col);
                std::string nmsg = newMessages(origMsgs, rebuilt);
                check(nmsg.empty(), name + " no new parser messages", nmsg);
                if (k == 0) writeFile(outdir + "/T5_shrink64.bin", rebuilt);
            }
            else {
                // A clean refusal is acceptable for shrinks that can't be padded; it must carry a message
                check(!msgs.empty(), name + " refused cleanly", std::string(errorCodeToUString(r).toLocal8Bit()) + (msgs.empty() ? "" : " | " + std::string(msgs.back().toLocal8Bit())));
            }
        }
    }

    // ---- T6: body larger than anything available must be refused with a message and no output
    {
        UByteArray big = vbtBody + UByteArray((size_t)(4 * 1024 * 1024), '\x5A');
        UByteArray rebuilt, c = big; msgs.clear();
        USTATUS r = rep.replace(image, vbtGuid, EFI_SECTION_RAW, c, false, true, rebuilt, msgs);
        check(r != U_SUCCESS && rebuilt.isEmpty(), "T6 +4MiB body is refused, no output produced", errorCodeToUString(r).toLocal8Bit());
        for (size_t i = 0; i < msgs.size(); i++) std::cout << "    msg: " << msgs[i].toLocal8Bit() << std::endl;
    }

    // ---- T7: modest growth (+16 bytes); either builds + re-parses, or is cleanly refused
    {
        UByteArray grown = vbtBody + UByteArray((size_t)16, '\0');
        UByteArray rebuilt, c = grown; msgs.clear();
        USTATUS r = rep.replace(image, vbtGuid, EFI_SECTION_RAW, c, false, true, rebuilt, msgs);
        if (r == U_SUCCESS) {
            TreeModel m; FfsParser p(&m);
            bool ok = p.parse(rebuilt) == U_SUCCESS && rebuilt.size() == image.size();
            UModelIndex s = ok ? findFileSection(m, m.index(0, 0), vbtGuid, EFI_SECTION_RAW) : UModelIndex();
            check(ok && s.isValid() && m.body(s) == grown, "T7 +16 bytes builds and re-parses");
            int nf = 0; std::string col = collateral(image, rebuilt, nf);
            check(col.empty(), "T7 collateral: all other files (" + std::to_string(nf) + ") byte-identical", col);
            std::string nmsg = newMessages(origMsgs, rebuilt);
            check(nmsg.empty(), "T7 no new parser messages", nmsg);
        }
        else check(!msgs.empty(), "T7 +16 bytes refused cleanly", std::string(errorCodeToUString(r).toLocal8Bit()) + (msgs.empty() ? "" : " | " + std::string(msgs.back().toLocal8Bit())));
    }


    // ---- T8: modifying a TE/PE image of a PEI-phase file in an uncompressed volume must be refused (needs rebase)
    {
        EFI_GUID dg; ustringToGuid(UString("86D70125-BAA3-4296-A62F-602BEBBB9081"), dg); // DxeIpl in Cyborg-STOCK
        UByteArray dxeIplGuid((const char*)&dg, sizeof(EFI_GUID));
        TreeModel m; FfsParser p(&m); p.parse(image);
        UModelIndex s = findFileSection(m, m.index(0, 0), dxeIplGuid, EFI_SECTION_TE);
        if (!s.isValid()) {
            std::cout << "[SKIP] T8 DxeIpl TE section not found in this image" << std::endl;
        }
        else {
            UByteArray mod = m.body(s); mod[mod.size() - 1] ^= 1;
            UByteArray rebuilt; msgs.clear();
            USTATUS r = rep.replace(image, dxeIplGuid, EFI_SECTION_TE, mod, false, true, rebuilt, msgs);
            check(r == U_REBASE_REQUIRED && rebuilt.isEmpty(), "T8 TE edit in PEI-phase file refused with REBASE_REQUIRED", errorCodeToUString(r).toLocal8Bit());
        }
    }

    // ---- T9: edit inside an EFI 1.1 compressed section (forces recompression), then re-parse
    {
        EFI_GUID sg; ustringToGuid(UString("4E22BF36-C59D-47C4-9FD1-25A603D75958"), sg); // SaInitFsp in Cyborg-STOCK
        UByteArray saGuid((const char*)&sg, sizeof(EFI_GUID));
        TreeModel m; FfsParser p(&m); p.parse(image);
        UModelIndex s = findFileSection(m, m.index(0, 0), saGuid, EFI_SECTION_USER_INTERFACE);
        if (!s.isValid()) {
            std::cout << "[SKIP] T9 SaInitFsp UI section not found in this image" << std::endl;
        }
        else {
            UByteArray mod = m.body(s); mod[0] = 'Z';
            UByteArray rebuilt; msgs.clear();
            USTATUS r = rep.replace(image, saGuid, EFI_SECTION_USER_INTERFACE, mod, false, true, rebuilt, msgs);
            check(r == U_SUCCESS, "T9 UI-section edit inside EFI 1.1 compressed section builds", r ? errorCodeToUString(r).toLocal8Bit() : "");
            if (r == U_SUCCESS) {
                TreeModel m2; FfsParser p2(&m2);
                bool ok = p2.parse(rebuilt) == U_SUCCESS && rebuilt.size() == image.size();
                UModelIndex s2 = ok ? findFileSection(m2, m2.index(0, 0), saGuid, EFI_SECTION_USER_INTERFACE) : UModelIndex();
                check(ok && s2.isValid() && m2.body(s2) == mod, "T9 re-parsed output contains the edited UI section");
                std::string nmsg = newMessages(origMsgs, rebuilt);
                check(nmsg.empty(), "T9 no new parser messages", nmsg);
            }
        }
    }

    std::cout << "FAILURES: " << failures << std::endl;
    return failures;
}
