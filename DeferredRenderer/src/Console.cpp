#include "Console.h"
#include <iostream>

namespace dr {

Console::~Console() {
    RequestStop();
    if (m_thread.joinable()) m_thread.detach();
}

void Console::Start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { Loop(); });
}

void Console::RequestStop() { m_running.store(false); }

bool Console::TryPop(std::string& outLine) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_queue.empty()) return false;
    outLine = std::move(m_queue.front());
    m_queue.pop();
    return true;
}

void Console::Loop() {
    std::string line;
    while (m_running.load() && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_queue.push(std::move(line));
    }
}

} // namespace dr
