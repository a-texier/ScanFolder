#include "scan.hpp"
#include "delete.hpp"
#include "view.hpp"
#include "operations.hpp"
#include "archive.hpp"
#include "interaction.hpp"
#include "cache.hpp"
#include "store.hpp"
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <sstream>
#include <thread>

namespace sf {
namespace {
bool writeFile(const std::wstring& path, DWORD bytes, bool readOnly = false) {
    HANDLE h = CreateFileW(extended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string data(bytes, 'x'); DWORD written = 0; bool ok = WriteFile(h, data.data(), bytes, &written, nullptr) && written == bytes;
    CloseHandle(h); if (readOnly) SetFileAttributesW(extended(path).c_str(), FILE_ATTRIBUTE_READONLY); return ok;
}
bool makeDir(const std::wstring& path) { return CreateDirectoryW(extended(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS; }
bool junction(const std::wstring& link, const std::wstring& target) {
    if (!makeDir(link)) return false;
    HANDLE h = CreateFileW(extended(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring substitute = L"\\??\\" + target, print = target;
    std::vector<BYTE> b(16 + (substitute.size() + print.size() + 2) * 2, 0);
    DWORD tag = IO_REPARSE_TAG_MOUNT_POINT; memcpy(b.data(), &tag, 4);
    auto put = [&](size_t p, WORD v) { memcpy(b.data() + p, &v, 2); };
    put(4, WORD(b.size() - 8)); put(8, 0); put(10, WORD(substitute.size() * 2));
    put(12, WORD((substitute.size() + 1) * 2)); put(14, WORD(print.size() * 2));
    memcpy(b.data() + 16, substitute.c_str(), (substitute.size() + 1) * 2);
    memcpy(b.data() + 16 + (substitute.size() + 1) * 2, print.c_str(), (print.size() + 1) * 2);
    DWORD bytes = 0; bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, b.data(), DWORD(b.size()), nullptr, 0, &bytes, nullptr) != FALSE;
    CloseHandle(h); return ok;
}
}
int selfTest(const std::wstring& output) {
    std::ofstream report(std::filesystem::path(output), std::ios::binary); int failed = 0;
    auto check = [&](bool ok, const char* name) { report << (ok ? "PASS " : "FAIL ") << name << "\n"; report.flush(); if (!ok) ++failed; };
    check(mftParserTest(), "MFT fixups, signed runlist deltas and malformed runlist");
    check(viewTest(), "background views: sort, extension totals and case-insensitive filtering");
    check(operationPolicyTest(), "recycle sink rejects permanent fallback and honors cancellation");
    check(archivePolicyTest(), "ZIP command quoting and source boundary checks");
    check(interactionTest(),"Ctrl/Shift selection, right-click preservation, history state, overlapping selection roots");
    wchar_t temp[32768]{}; GetTempPathW(32768, temp);
    auto base = normalize(temp) + L"\\ScanFolder-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    auto fixture = base + L"\\fixture", outside = base + L"\\outside";
    check(makeDir(base) && makeDir(fixture) && makeDir(outside), "isolated temporary test directories");
    if (failed) return 1;
    auto begin = std::chrono::steady_clock::now();
    uint64_t expected = 0; unsigned expectedFiles = 0; bool created = true;
    for (int dir = 0; dir < 24; ++dir) {
        auto path = fixture + L"\\lot-" + std::to_wstring(dir); created &= makeDir(path);
        for (int file = 0; file < 1000; ++file) {
            DWORD size = DWORD((dir * 17 + file) % 113 + 1);
            created &= writeFile(path + L"\\fichier-" + std::to_wstring(file) + L".bin", size, file % 200 == 0);
            expected += size; ++expectedFiles;
        }
    }
    auto nested = fixture;
    for (int i = 0; i < 12; ++i) { nested += L"\\Données-éà_日本_" + std::to_wstring(i); created &= makeDir(nested); }
    created &= writeFile(nested + L"\\résultat-中文.txt", 987); expected += 987; ++expectedFiles;
    check(created, "24,001 files, readonly entries, Unicode names and long paths");
    bool linked = junction(fixture + L"\\junction-outside", outside);
    check(linked, "directory junction fixture created");
    check(writeFile(outside + L"\\KEEP.txt", 999), "junction target sentinel created");
    double creation = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    report << "fixture_creation_seconds=" << creation << "\n";
    Progress p; Options opt; opt.mft = false; opt.workers = 4;
    auto r = scan(fixture, p, opt);
    check(!r->nodes.empty() && r->nodes[0].files == expectedFiles, "parallel scan file count (junction target excluded)");
    check(!r->nodes.empty() && r->nodes[0].total == expected, "parallel scan logical bytes exact");
    check(r->errors == 0 && r->skippedLinks == (linked ? 1u : 0u), "scan has no errors; junction not followed");
    report << "scan_workers=4\nscan_seconds=" << r->seconds << "\nscan_files=" << expectedFiles << "\nscan_files_per_second=" << expectedFiles / std::max(.0001,r->seconds) << "\n";
    auto csv = base + L"\\scan.csv"; std::wstring error;
    check(exportCsv(*r, csv, error), "CSV export including Unicode and numeric byte columns");
    auto database=base+L"\\index.db";auto saveBegin=std::chrono::steady_clock::now();bool saved=saveDatabase(*r,database,error);
    report<<"db_save_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-saveBegin).count()<<"\n";
    auto loaded=loadDatabase(database,error);
    check(saved&&loaded&&loaded->nodes[0].total==r->nodes[0].total&&loaded->nodes[0].files==expectedFiles&&loaded->nodes.size()==r->nodes.size(),"compressed database roundtrip with Unicode, long paths and 24,001 files");
    if(loaded)report<<"db_load_seconds="<<loaded->seconds<<"\n";
    report<<"db_bytes="<<std::filesystem::file_size(database)<<"\n";
    auto invalid=base+L"\\invalid.db";std::filesystem::copy_file(database,invalid);
    {std::fstream corrupt(std::filesystem::path(invalid),std::ios::in|std::ios::out|std::ios::binary);corrupt.seekg(-1,std::ios::end);char b=0;corrupt.read(&b,1);b=static_cast<char>(static_cast<unsigned char>(b)^0x5a);corrupt.seekp(-1,std::ios::end);corrupt.write(&b,1);}
    check(!loadDatabase(invalid,error),"corrupted compressed database rejected without trusting cached paths");
    {
        // All persistence tests use their own cache directory, never the user's index.
        wchar_t previous[32768]{};DWORD hadPrevious=GetEnvironmentVariableW(L"SCANFOLDER_CACHE_DIR",previous,32768);
        auto isolated=base+L"\\multi-cache";makeDir(isolated);SetEnvironmentVariableW(L"SCANFOLDER_CACHE_DIR",isolated.c_str());
        auto other=std::make_shared<Result>();other->root=outside;other->nodes.resize(1);other->nodes[0].directory=true;other->aggregate();
        DiskWriter writer;writer.save(r,[]{});writer.save(other,[]{});writer.save(r,[]{});writer.waitIdle();
        auto first=loadCachedIndex(fixture,error),second=loadCachedIndex(outside,error);
        check(first&&second&&first->nodes[0].files==expectedFiles&&second->nodes[0].files==0&&databasePath(fixture)!=databasePath(outside),"queued saves retain separate indexes for two roots (no last-scan overwrite)");
        check(loadCachedIndex(nested,error)&&!loadCachedIndex(base+L"\\never-scanned",error),"cached parent opens nested paths; an unscanned root stays empty without a scan");
        SessionState s1;s1.root=fixture;s1.directory=nested;s1.query=L"résultat";s1.scroll=42;s1.tab=1;
        SessionState s2;s2.root=outside;s2.directory=outside;s2.scroll=7;
        check(saveSession(s1)&&saveSession(s2),"save independent per-root sessions");
        auto state1=loadSession(fixture),state2=loadSession(outside),last=loadSession();
        check(state1&&state2&&last&&state1->directory==nested&&state1->query==s1.query&&state1->scroll==42&&state2->scroll==7&&last->root==outside,"switching back restores each root's folder, filter and scroll, including after restart");
        auto newer=std::make_shared<Result>(*other);newer->nodes[0].name=L"replacement";writer.save(newer,[]{});writer.waitIdle();
        first=loadCachedIndex(fixture,error);second=loadCachedIndex(outside,error);
        check(first&&second&&first->nodes[0].files==expectedFiles&&second->nodes[0].name==L"replacement","refresh replaces only the selected root's database");
        auto sentinel=isolated+L"\\user-document.txt";writeFile(sentinel,5);
        writer.clear([]{});writer.waitIdle();
        check(!loadCachedIndex(fixture,error)&&!loadCachedIndex(outside,error)&&!loadSession()&&cacheBytes()==0&&GetFileAttributesW(sentinel.c_str())!=INVALID_FILE_ATTRIBUTES,"clear removes all app indexes and sessions while preserving unrelated files");
        check(saveDatabase(*r,databasePath(),error)&&loadCachedIndex(nested,error)&&!loadCachedIndex(outside,error),"legacy single cache is reused only for its own subtree");
        writer.shutdown();SetEnvironmentVariableW(L"SCANFOLDER_CACHE_DIR",hadPrevious?previous:nullptr);
    }
    auto cacheRoot=base+L"\\cache-fixture",keep=cacheRoot+L"\\keep",view=cacheRoot+L"\\view";
    makeDir(cacheRoot);makeDir(keep);makeDir(view);writeFile(keep+L"\\untouched.bin",127);writeFile(view+L"\\a.bin",11);writeFile(view+L"\\b.bin",22);writeFile(view+L"\\c.bin",33);
    Progress initialCacheProgress;auto index=scan(cacheRoot,initialCacheProgress,opt);auto viewId=findCached(*index,view),cId=findCached(*index,view+L"\\c.bin"),keepId=findCached(*index,keep+L"\\untouched.bin");
    DeleteProgress batchProgress;auto batch=deleteBatch({view+L"\\a.bin",view+L"\\b.bin"},batchProgress,4);
    check(batch.errors==0&&batch.files==2,"batch permanent deletion reports two selected files exactly once");
    {HANDLE modify=CreateFileW(extended(keep+L"\\untouched.bin").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);DWORD count=0;WriteFile(modify,"extra",5,&count,nullptr);CloseHandle(modify);}
    makeDir(view+L"\\new");writeFile(view+L"\\new\\added.bin",44);
    CacheChanges changes;changes.changed={view+L"\\a.bin",view+L"\\b.bin"};changes.parents={view};Progress refreshProgress;
    auto refreshed=refreshCache(index,changes,refreshProgress,4);
    check(refreshed&&findCached(*refreshed,view)==viewId&&findCached(*refreshed,view+L"\\c.bin")==cId&&refreshed->nodes[0].total==204&&refreshed->nodes[0].files==3,"local cache refresh retains current folder IDs and correct totals after multi-delete and copy");
    check(refreshed&&refreshed->nodes[keepId].total==127,"unrelated folder is never rescanned during local refresh");
    auto compactDb=base+L"\\compacted.db";saved=refreshed&&saveDatabase(*refreshed,compactDb,error);auto compacted=loadDatabase(compactDb,error);
    check(saved&&compacted&&compacted->nodes[0].total==204&&compacted->nodes.size()<refreshed->nodes.size(),"database removes deleted tombstones while preserving live index");
    auto opSource=base+L"\\copie-source",opDest=base+L"\\copie-destination",moveDest=base+L"\\deplacement";
    makeDir(opSource);makeDir(opDest);makeDir(moveDest);writeFile(opSource+L"\\données_日本.txt",321);
    DeleteProgress copyProgress;auto copied=shellOperation(nullptr,{opSource},opDest,false,false,copyProgress);
    check(!copied.errors&&GetFileAttributesW(extended(opDest+L"\\copie-source\\données_日本.txt").c_str())!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(extended(opSource).c_str())!=INVALID_FILE_ATTRIBUTES,"Windows copy preserves source and Unicode file");
    DeleteProgress moveProgress;auto moved=shellOperation(nullptr,{opDest+L"\\copie-source"},moveDest,true,false,moveProgress);
    check(!moved.errors&&GetFileAttributesW(extended(moveDest+L"\\copie-source\\données_日本.txt").c_str())!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(extended(opDest+L"\\copie-source").c_str())==INVALID_FILE_ATTRIBUTES,"Windows move preserves data and removes old entry");
    DeleteProgress collisionProgress;auto collision=shellOperation(nullptr,{opSource},moveDest,false,false,collisionProgress);
    check(!collision.errors&&GetFileAttributesW(extended(moveDest+L"\\copie-source\\données_日本.txt").c_str())!=INVALID_FILE_ATTRIBUTES,"copy collision retains existing destination");
    auto zipPath=base+L"\\résultat 日本.zip";DeleteProgress zipProgress;auto zip=createZip(fixture,zipPath,false,zipProgress,4,1);
    check(zip.success&&zip.files==expectedFiles&&zip.skipped==(linked?1u:0u),"embedded 7-Zip: fast ZIP, Unicode names, long paths, CRC verification, junction excluded");
    report<<"zip_seconds="<<zip.seconds<<"\n";if(!zip.success){auto msg=zip.message;for(wchar_t c:msg)report<<(c<128?char(c):'?');report<<"\n";}
    DeleteProgress noOverwrite;auto preservedZip=createZip(fixture,zipPath,false,noOverwrite,4,1);
    check(!preservedZip.success&&GetFileAttributesW(extended(zipPath).c_str())!=INVALID_FILE_ATTRIBUTES,"ZIP refuses existing destination without replace authorization");
    DeleteProgress invalidZip;auto nestedZip=createZip(fixture,fixture+L"\\invalid.zip",false,invalidZip,4,1);
    check(!nestedZip.success&&GetFileAttributesW(extended(fixture+L"\\invalid.zip").c_str())==INVALID_FILE_ATTRIBUTES,"ZIP inside source rejected");
    DeleteProgress zipCancel;zipCancel.cancel=true;auto cancelledZip=createZip(fixture,base+L"\\cancelled.zip",false,zipCancel,4,1);
    check(cancelledZip.cancelled&&GetFileAttributesW(extended(base+L"\\cancelled.zip").c_str())==INVALID_FILE_ATTRIBUTES,"ZIP cancellation never publishes incomplete output");
    uint32_t unicode = none;
    for (uint32_t i = 0; i < r->nodes.size(); ++i) if (r->nodes[i].name == L"résultat-中文.txt") unicode = i;
    check(unicode != none && r->path(unicode) == nested + L"\\résultat-中文.txt", "parent reconstruction for nested long path");
    Progress cancelled; cancelled.cancel = true; auto stopped = scan(fixture, cancelled, opt);
    check(stopped->cancelled && cancelled.files == 0, "scan cancellation before work");
    wchar_t volume[MAX_PATH]{}; GetVolumePathNameW(base.c_str(), volume, MAX_PATH);
    check(!validateDelete(volume, error), "volume root deletion blocked");
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    check(!validateDelete(windows, error), "Windows directory deletion blocked");
    DeleteProgress noDelete; noDelete.cancel = true; auto notDeleted = deletePermanently(fixture, noDelete, 4);
    check(notDeleted.cancelled && GetFileAttributesW(extended(nested + L"\\résultat-中文.txt").c_str()) != INVALID_FILE_ATTRIBUTES, "delete cancellation leaves files intact");
    // A handle without FILE_SHARE_DELETE must be reported, never hidden as success.
    auto locked = fixture + L"\\locked.txt"; writeFile(locked, 12);
    HANDLE hold = CreateFileW(extended(locked).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DeleteProgress dp; auto deletion = deletePermanently(fixture, dp, 4);
    check(deletion.errors > 0 && GetFileAttributesW(extended(locked).c_str()) != INVALID_FILE_ATTRIBUTES, "locked file is preserved and reported as failure");
    check(GetFileAttributesW(extended(outside + L"\\KEEP.txt").c_str()) != INVALID_FILE_ATTRIBUTES, "parallel deletion never follows directory junction target");
    check(deletion.files == expectedFiles, "parallel deletion removes readonly, Unicode and long-path files");
    report << "delete_workers=4\ndelete_seconds=" << deletion.seconds << "\ndelete_files=" << deletion.files << "\ndelete_files_per_second=" << deletion.files / std::max(.0001,deletion.seconds) << "\n";
    if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
    DeleteProgress cleanup; auto finish = deletePermanently(base, cleanup, 4);
    check(finish.errors == 0 && GetFileAttributesW(extended(base).c_str()) == INVALID_FILE_ATTRIBUTES, "isolated fixture cleanup after releasing locked file");
    report << "FAILURES=" << failed << "\n"; return failed ? 1 : 0;
}
}
