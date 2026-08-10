#pragma once

namespace dr {

struct Input {
    bool keys[256] = {};
    int  mouseDX     = 0;
    int  mouseDY     = 0;
    bool lmb         = false;
    bool rmb         = false;
    bool lookActive  = false;  // true while in Alt-toggle look mode (cursor locked + hidden)

    void NewFrame() {
        mouseDX = 0;
        mouseDY = 0;
    }
};

} // namespace dr
