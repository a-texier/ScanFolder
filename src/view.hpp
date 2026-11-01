#pragma once
#include "scan.hpp"
#include <condition_variable>
#include <optional>
#include <thread>
namespace sf {
struct FileType { std::wstring name; uint64_t bytes = 0; uint32_t count = 0; };
struct ViewRequest {
    std::shared_ptr<const Result> result;
    uint32_t current = 0; int tab = 0, sort = 1;
    bool descending = true, allocated = false;
    std::wstring query, extension;
    uint64_t generation = 0;
};
struct ViewOutput { std::vector<uint32_t> rows; std::vector<FileType> types; uint64_t generation = 0; std::wstring error; };
class ViewWorker {
    std::mutex mutex; std::condition_variable cv;
    std::optional<ViewRequest> pending;
    std::optional<ViewOutput> ready;
    std::atomic<uint64_t> generation{0};
    bool stopping = false; std::thread thread;
    std::function<void()> notify;
    void loop();
public:
    ~ViewWorker();
    uint64_t submit(ViewRequest request, std::function<void()> callback);
    std::optional<ViewOutput> take();
    void cancel(); void shutdown();
};
bool viewTest();
}
