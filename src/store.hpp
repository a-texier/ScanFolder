#pragma once
#include "scan.hpp"
#include <condition_variable>
#include <deque>
#include <thread>
#include <optional>
namespace sf {
bool saveDatabase(const Result&,const std::wstring&,std::wstring&);
std::shared_ptr<Result> loadDatabase(const std::wstring&,std::wstring&);
std::wstring databasePath();
std::wstring databasePath(const std::wstring& root);
std::shared_ptr<Result> loadCachedIndex(const std::wstring& target,std::wstring& error);
uint64_t cacheBytes();
struct SessionState { std::wstring root,directory,query,extension;uint64_t scroll=0;int tab=0,sort=1;bool descending=true,allocated=false; };
bool saveSession(const SessionState&);
std::optional<SessionState> loadSession(const std::wstring& root=L"");
class DiskWriter {
    struct Work { std::shared_ptr<const Result> result;bool clear=false;std::wstring path;std::function<void()> notify; };
    std::mutex mutex;std::condition_variable cv;std::deque<Work> queue;std::thread thread;bool stopping=false,writing=false;
    void loop();
public:
    std::atomic<uint64_t> bytes{0};
    std::wstring error;
    ~DiskWriter();
    void save(std::shared_ptr<const Result>,std::function<void()>);
    void clear(std::function<void()>);
    std::wstring lastError();
    void waitIdle();
    void shutdown();
};
}
