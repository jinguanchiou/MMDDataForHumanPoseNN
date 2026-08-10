#include "Scene.h"
#include <cwctype>

namespace dr {

const char* RenderProfileName(RenderProfile p) {
    switch (p) {
        case RenderProfile::EndfieldPBR: return "EndfieldPBR";
        case RenderProfile::WuwaPBR:     return "WuwaPBR";
        case RenderProfile::ZzzNPR:      return "ZzzNPR";
        default:                         return "Cel";
    }
}

// Folder → profile: match the game name anywhere in the path. Add a game here to route its
// per-game folder to a render method. Keep the character/MMD default (Cel) last.
RenderProfile ProfileForPath(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p) c = static_cast<wchar_t>(towlower(c));   // ASCII keywords; CJK is unaffected
    auto has = [&](const wchar_t* kw) { return p.find(kw) != std::wstring::npos; };

    if (has(L"终末地") || has(L"終末地") || has(L"endfield") || has(L"明日方舟") || has(L"arknights"))
        return RenderProfile::EndfieldPBR;
    if (has(L"鸣潮") || has(L"鳴潮") || has(L"wuwa") || has(L"wuthering"))
        return RenderProfile::WuwaPBR;
    if (has(L"绝区零") || has(L"絕區零") || has(L"zzz") || has(L"zenless"))
        return RenderProfile::ZzzNPR;
    return RenderProfile::Cel;
}

} // namespace dr
