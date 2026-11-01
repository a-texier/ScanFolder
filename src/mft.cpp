#include "scan.hpp"
#include <winioctl.h>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace sf {
namespace {
template<class T> T read(const uint8_t* p) { T v{}; memcpy(&v, p, sizeof v); return v; }
struct Run { uint64_t vcn, lcn, count; };
struct Name { uint64_t parent; std::wstring name; uint8_t space; };
struct Record { std::vector<Name> names; uint64_t size = 0, allocated = 0, modified = 0; bool directory = false, reparse = false; };
bool fixup(std::vector<uint8_t>& b, unsigned sector) {
    if (b.size() < 48 || read<uint32_t>(b.data()) != 0x454c4946 || sector < 256) return false;
    unsigned off = read<uint16_t>(&b[4]), count = read<uint16_t>(&b[6]);
    if (count != b.size() / sector + 1 || off + count * 2 > b.size()) return false;
    auto sequence = read<uint16_t>(&b[off]);
    for (unsigned i = 1; i < count; ++i) {
        auto end = size_t(i) * sector - 2;
        if (end + 2 > b.size() || read<uint16_t>(&b[end]) != sequence) return false;
        memcpy(&b[end], &b[off + 2 * i], 2);
    }
    return true;
}
template<class F> bool attributes(const std::vector<uint8_t>& b, F f) {
    size_t off = read<uint16_t>(&b[20]);
    size_t limit = std::min<size_t>(read<uint32_t>(&b[24]), b.size());
    for (unsigned guard = 0; off + 8 <= limit && guard < 1024; ++guard) {
        auto a = &b[off]; uint32_t type = read<uint32_t>(a), length = read<uint32_t>(a + 4);
        if (type == UINT32_MAX) return true;
        if (length < 24 || length > limit - off) return false;
        f(type, a, length); off += length;
    }
    return false;
}
bool parseRecord(const std::vector<uint8_t>& b, Record& out) {
    out.directory = (read<uint16_t>(&b[22]) & 2) != 0;
    return attributes(b, [&](uint32_t type, const uint8_t* a, uint32_t length) {
        if (type == 0x80 && a[9] == 0) {
            if (a[8] && length >= 64 && read<uint64_t>(a + 16) == 0) {
                out.size = read<uint64_t>(a + 48); out.allocated = read<uint64_t>(a + 40);
                if ((read<uint16_t>(a + 12) & 0x8001) && read<uint16_t>(a + 32) >= 72 && length >= 72) out.allocated = read<uint64_t>(a + 64);
            } else if (!a[8]) out.size = read<uint32_t>(a + 16);
        }
        if (a[8]) return;
        auto off = read<uint16_t>(a + 20); auto len = read<uint32_t>(a + 16);
        if (off > length || len > length - off) return;
        auto value = a + off;
        if (type == 0x10 && len >= 36) {
            out.modified = read<uint64_t>(value + 8);
            out.reparse = (read<uint32_t>(value + 32) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        }
        if (type == 0x30 && len >= 66 && 66u + unsigned(value[64]) * 2 <= len) {
            Name n{read<uint64_t>(value) & 0x0000ffffffffffffULL, {}, value[65]};
            n.name.resize(value[64]); memcpy(n.name.data(), value + 66, value[64] * 2); out.names.push_back(std::move(n));
        }
    });
}
bool runlist(const uint8_t* a, uint32_t len, std::vector<Run>& runs) {
    if (len < 64 || !a[8]) return false;
    size_t p = read<uint16_t>(a + 32); uint64_t vcn = read<uint64_t>(a + 16); int64_t lcn = 0;
    while (p < len && a[p]) {
        unsigned sizes = a[p++], lc = sizes & 15, oc = sizes >> 4;
        if (!lc || lc > 8 || !oc || oc > 8 || p + lc + oc > len) return false;
        uint64_t count = 0, delta = 0;
        for (unsigned i = 0; i < lc; ++i) count |= uint64_t(a[p++]) << (i * 8);
        for (unsigned i = 0; i < oc; ++i) delta |= uint64_t(a[p++]) << (i * 8);
        if (oc < 8 && (delta & (uint64_t(1) << (oc * 8 - 1)))) delta |= UINT64_MAX << (oc * 8);
        lcn += static_cast<int64_t>(delta);
        if (!count || lcn < 0 || count > UINT64_MAX - vcn) return false;
        runs.push_back({vcn, uint64_t(lcn), count}); vcn += count;
    }
    return p < len;
}
bool readAt(HANDLE h, uint64_t offset, void* data, DWORD count) {
    LARGE_INTEGER pos; pos.QuadPart = offset; DWORD got = 0;
    return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ReadFile(h, data, count, &got, nullptr) && got == count;
}
}
using RawReader = std::function<bool(uint64_t, void*, DWORD)>;
static bool readMft(const RawReader&, const NTFS_VOLUME_DATA_BUFFER&, const std::wstring&, Progress&, Result&);
bool scanMft(const std::wstring& root, Progress& progress, Result& result) {
    if (root.size() < 3 || root[1] != L':' || root[2] != L'\\') return false;
    wchar_t mount[MAX_PATH]{}; wchar_t fs[MAX_PATH]{};
    if (!GetVolumePathNameW(root.c_str(), mount, MAX_PATH) || wcslen(mount) != 3 ||
        !GetVolumeInformationW(mount, nullptr, 0, nullptr, nullptr, nullptr, fs, MAX_PATH) || _wcsicmp(fs, L"NTFS")) return false;
    std::wstring device = L"\\\\.\\" + root.substr(0, 2);
    HANDLE h = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        result.note = L"Turbo NTFS indisponible sans accès administrateur. Parcours standard utilisé."; return false;
    }
    struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{h};
    NTFS_VOLUME_DATA_BUFFER info{}; DWORD bytes = 0;
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &info, sizeof info, &bytes, nullptr)) return false;
    return readMft([&](uint64_t offset,void* data,DWORD count){return readAt(h,offset,data,count);},info,root,progress,result);
}
static bool readMft(const RawReader& reader, const NTFS_VOLUME_DATA_BUFFER& info, const std::wstring& root, Progress& progress, Result& result) {
    const unsigned recordSize = info.BytesPerFileRecordSegment, sector = info.BytesPerSector;
    if (recordSize < 512 || recordSize > 65536 || !sector || !info.BytesPerCluster || info.MftValidDataLength.QuadPart <= 0) return false;
    std::vector<uint8_t> record(recordSize);
    if (!reader(uint64_t(info.MftStartLcn.QuadPart) * info.BytesPerCluster, record.data(), recordSize) || !fixup(record, sector)) return false;
    std::vector<Run> runs; bool validRuns = true;
    if (!attributes(record, [&](uint32_t type, const uint8_t* a, uint32_t len) {
        if (type == 0x80 && a[9] == 0) validRuns &= runlist(a, len, runs);
    }) || !validRuns || runs.empty()) return false;
    std::sort(runs.begin(), runs.end(), [](auto& a, auto& b) { return a.vcn < b.vcn; });
    uint64_t covered = 0;
    for (auto& run : runs) { if (run.vcn != covered) return false; covered += run.count; }
    if (covered * info.BytesPerCluster < uint64_t(info.MftValidDataLength.QuadPart)) {
        result.note = L"MFT répartie sur des attributs supplémentaires : parcours standard utilisé."; return false;
    }
    progress.status(L"Lecture rapide de la MFT", root);
    Result all; all.root = root.substr(0, 3);
    Node top; top.name = all.root; top.directory = true; all.nodes.push_back(top);
    uint64_t nRecords = uint64_t(info.MftValidDataLength.QuadPart) / recordSize;
    if (nRecords > 200000000) return false;
    std::vector<uint32_t> directories(size_t(nRecords), none); if (nRecords > 5) directories[5] = 0;
    std::vector<uint64_t> parents{0}, ids{0};
    all.nodes.reserve(size_t(std::min<uint64_t>(nRecords, 1000000)));
    std::vector<uint8_t> block(4 * 1024 * 1024);
    for (auto& run : runs) {
        uint64_t runStart = run.vcn * info.BytesPerCluster, runBytes = run.count * info.BytesPerCluster;
        uint64_t available = uint64_t(info.MftValidDataLength.QuadPart);
        if (runStart >= available) break;
        runBytes = std::min(runBytes, available - runStart);
        for (uint64_t off = 0; off < runBytes && !progress.cancel; ) {
            DWORD count = DWORD(std::min<uint64_t>(block.size(), runBytes - off));
            count -= count % recordSize;
            if (!count || !reader(run.lcn * info.BytesPerCluster + off, block.data(), count)) return false;
            for (DWORD p = 0; p < count; p += recordSize) {
                if (progress.cancel) break;
                uint64_t id = (runStart + off + p) / recordSize; ++progress.records;
                memcpy(record.data(), block.data() + p, recordSize);
                if (read<uint32_t>(record.data()) != 0x454c4946 || !(read<uint16_t>(&record[22]) & 1)) continue;
                if (!fixup(record, sector)) { ++progress.errors; continue; }
                if (id < 16 || read<uint64_t>(&record[32])) continue;
                Record parsed;
                if (!parseRecord(record, parsed)) { ++progress.errors; continue; }
                bool longName = std::any_of(parsed.names.begin(), parsed.names.end(), [](auto& n) { return n.space == 1 || n.space == 3; });
                for (auto& name : parsed.names) {
                    if ((longName && name.space == 2) || name.name == L"." || name.parent >= nRecords) continue;
                    Node n; n.name = std::move(name.name); n.directory = parsed.directory; n.reparse = parsed.reparse;
                    n.size = n.directory ? 0 : parsed.size; n.allocated = n.directory ? 0 : parsed.allocated; n.modified = parsed.modified;
                    uint32_t idx = uint32_t(all.nodes.size());
                    if (n.directory) { if (directories[size_t(id)] != none) continue; directories[size_t(id)] = idx; }
                    all.nodes.push_back(std::move(n)); parents.push_back(name.parent); ids.push_back(id);
                    if (parsed.directory) ++progress.folders; else { ++progress.files; progress.bytes += parsed.size; }
                }
            }
            off += count;
        }
        if (progress.cancel) break;
    }
    if (progress.cancel) { result.note = L"Lecture MFT annulée avant reconstruction."; return false; }
    for (uint32_t i = 1; i < all.nodes.size(); ++i) {
        auto parent = directories[size_t(parents[i])];
        if (parent == none || parent == i || (parent && all.nodes[parent].reparse)) continue;
        all.nodes[i].parent = parent; all.nodes[i].nextSibling = all.nodes[parent].firstChild; all.nodes[parent].firstChild = i;
    }
    uint32_t selected = 0;
    for (size_t p = 3; p < root.size();) {
        auto end = root.find(L'\\', p); if (end == std::wstring::npos) end = root.size();
        auto part = root.substr(p, end - p); uint32_t match = none;
        for (auto c = all.nodes[selected].firstChild; c != none; c = all.nodes[c].nextSibling)
            if (all.nodes[c].directory && _wcsicmp(all.nodes[c].name.c_str(), part.c_str()) == 0) { match = c; break; }
        if (match == none) return false;
        selected = match; p = end + 1;
    }
    // Compact only the requested subtree. Duplicate hardlinks keep their names, but allocation is counted once.
    result.nodes.clear(); Node base = all.nodes[selected]; base.parent = base.firstChild = base.nextSibling = none; result.nodes.push_back(base);
    std::vector<std::pair<uint32_t, uint32_t>> queue{{selected, 0}};
    std::unordered_set<uint64_t> seen;
    for (size_t p = 0; p < queue.size(); ++p) {
        auto [source, dest] = queue[p];
        for (auto c = all.nodes[source].firstChild; c != none; c = all.nodes[c].nextSibling) {
            uint32_t idx = uint32_t(result.nodes.size()); Node n = all.nodes[c];
            n.parent = dest; n.firstChild = none; n.nextSibling = result.nodes[dest].firstChild;
            if (!n.directory && !seen.insert(ids[c]).second) n.allocated = 0;
            result.nodes[dest].firstChild = idx; result.nodes.push_back(std::move(n));
            if (all.nodes[c].directory && !all.nodes[c].reparse) queue.push_back({c, idx});
            else if (all.nodes[c].directory) ++result.skippedLinks;
        }
    }
    result.method = L"NTFS turbo · lecture directe MFT";
    result.note = L"Liens physiques : allocation comptée une fois. Jonctions non parcourues. Les flux alternatifs ne sont pas inclus.";
    return true;
}
bool mftParserTest() {
    std::vector<uint8_t> b(1024, 0);
    auto put16 = [&](int p, uint16_t v) { memcpy(&b[p], &v, 2); };
    auto put32 = [&](int p, uint32_t v) { memcpy(&b[p], &v, 4); };
    put32(0, 0x454c4946); put16(4, 48); put16(6, 3); put16(48, 0xaaaa); put16(50, 0x1234); put16(52, 0x5678);
    put16(510, 0xaaaa); put16(1022, 0xaaaa);
    if (!fixup(b, 512) || read<uint16_t>(&b[510]) != 0x1234 || read<uint16_t>(&b[1022]) != 0x5678) return false;
    b[510] = 0; if (fixup(b, 512)) return false;
    uint8_t a[80]{}; a[8] = 1; a[32] = 64; a[64] = 0x11; a[65] = 3; a[66] = 10;
    a[67] = 0x11; a[68] = 2; a[69] = 0xfe; a[70] = 0;
    std::vector<Run> runs;
    if (!runlist(a, 80, runs) || runs.size() != 2 || runs[0].lcn != 10 || runs[1].lcn != 8 || runs[1].vcn != 3) return false;
    a[64] = 0x99; runs.clear(); if(runlist(a, 80, runs))return false;
    b.assign(1024,0);put16(20,56);put16(22,1);put32(24,256);
    put32(56,0x80);put32(60,72);b[64]=1;put16(88,72);
    uint64_t huge=0x100000123ULL,allocation=0x100001000ULL;memcpy(&b[104],&huge,8);memcpy(&b[96],&allocation,8);
    put32(128,0x30);put32(132,104);put32(144,70);put16(148,24);
    uint64_t parent=16;memcpy(&b[152],&parent,8);b[216]=2;b[217]=1;
    std::wstring name=L"é日";memcpy(&b[218],name.data(),4);put32(232,UINT32_MAX);
    Record parsed;if(!parseRecord(b,parsed)||parsed.size!=huge||parsed.allocated!=allocation||parsed.names.size()!=1||parsed.names[0].name!=name||parsed.names[0].parent!=16)return false;
    put32(132,2048);Record malformed;if(parseRecord(b,malformed))return false;
    // Exercise the entire MFT reader with an in-memory volume: a >4 GiB file, two
    // hardlink names, resident data, a junction with an excluded target, and an orphan.
    std::vector<uint8_t> image(4096+32*1024,0);
    auto record=[&](unsigned id,bool dir,const std::vector<std::pair<uint64_t,std::wstring>>& names,uint64_t size,bool link=false,bool mft=false){
        std::vector<uint8_t> frame(1024,0);
        auto w16=[&](size_t p,uint16_t v){memcpy(&frame[p],&v,2);};auto w32=[&](size_t p,uint32_t v){memcpy(&frame[p],&v,4);};auto w64=[&](size_t p,uint64_t v){memcpy(&frame[p],&v,8);};
        w32(0,0x454c4946);w16(4,48);w16(6,3);w16(20,56);w16(22,dir?3:1);w16(48,0xaaaa);w16(510,0xaaaa);w16(1022,0xaaaa);
        size_t pos=56;
        if(mft){w32(pos,0x80);w32(pos+4,80);frame[pos+8]=1;w16(pos+32,72);w64(pos+48,32*1024);w64(pos+40,32*1024);frame[pos+72]=0x11;frame[pos+73]=8;frame[pos+74]=1;pos+=80;}
        else if(!dir){w32(pos,0x80);w32(pos+4,72);frame[pos+8]=1;w64(pos+48,size);w64(pos+40,(size+4095)/4096*4096);pos+=72;}
        if(link){w32(pos,0x10);w32(pos+4,64);w32(pos+16,40);w16(pos+20,24);w32(pos+24+32,FILE_ATTRIBUTE_REPARSE_POINT);pos+=64;}
        for(auto& [parentId,filename]:names){auto length=(24+66+filename.size()*2+7)/8*8;w32(pos,0x30);w32(pos+4,uint32_t(length));w32(pos+16,uint32_t(66+filename.size()*2));w16(pos+20,24);w64(pos+24,parentId);frame[pos+24+64]=uint8_t(filename.size());frame[pos+24+65]=1;memcpy(&frame[pos+24+66],filename.data(),filename.size()*2);pos+=length;}
        w32(pos,UINT32_MAX);w32(24,uint32_t(pos+8));memcpy(image.data()+4096+id*1024,frame.data(),1024);
    };
    record(0,false,{},0,false,true);record(16,true,{{5,L"fixture"}},0);
    record(17,false,{{16,L"large.bin"},{16,L"hardlink.bin"}},huge);
    record(18,false,{{16,L"small.txt"}},17);record(20,true,{{16,L"junction"}},0,true);
    record(21,false,{{20,L"excluded.bin"}},999);record(22,false,{{31,L"orphan.bin"}},555);
    NTFS_VOLUME_DATA_BUFFER info{};info.BytesPerFileRecordSegment=1024;info.BytesPerSector=512;info.BytesPerCluster=4096;info.MftStartLcn.QuadPart=1;info.MftValidDataLength.QuadPart=32*1024;
    auto reader=[&](uint64_t offset,void* data,DWORD count){if(offset>image.size()||count>image.size()-offset)return false;memcpy(data,image.data()+offset,count);return true;};
    Progress progress;Result result;result.root=L"C:\\fixture";
    if(!readMft(reader,info,result.root,progress,result))return false;result.aggregate();
    if(result.nodes.empty()||result.nodes[0].files!=3||result.nodes[0].total!=huge*2+17||result.nodes[0].totalAllocated!=(huge+4095)/4096*4096+4096||result.skippedLinks!=1)return false;
    // A fragmented MFT whose extra data run is unavailable must cleanly fall back.
    image[4096+56+73]=1;Progress fallback;Result unsupported;return !readMft(reader,info,L"C:\\fixture",fallback,unsupported);
}
}
