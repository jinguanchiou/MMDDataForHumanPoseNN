#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>

namespace dr {

class Console {
public:
    Console() = default;
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    void Start();
    bool TryPop(std::string& outLine);
    void RequestStop();

private:
    void Loop();

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::queue<std::string> m_queue;
    std::atomic<bool>       m_running{ false };
};

} // namespace dr
