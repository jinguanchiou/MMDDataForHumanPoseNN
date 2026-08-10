#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <cstdio>

namespace dr {

using Microsoft::WRL::ComPtr;

inline std::string HrToString(HRESULT hr) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "HRESULT 0x%08X", static_cast<unsigned>(hr));
    return buf;
}

class HrException : public std::runtime_error {
public:
    explicit HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), m_hr(hr) {}
    HRESULT Code() const noexcept { return m_hr; }
private:
    HRESULT m_hr;
};

inline void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) throw HrException(hr);
}

template <class T>
inline void NameObject(T* obj, const wchar_t* name) {
    if (obj && name) obj->SetName(name);
}

template <typename T>
constexpr T AlignUp(T x, T a) { return (x + (a - 1)) & ~(a - 1); }

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace dr
