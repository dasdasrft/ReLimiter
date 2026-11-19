#pragma once
#include "config.h"

inline bool IsChinese() {
    return g_config.language == "zh-CN" || g_config.language == "zh" || g_config.language == "chinese";
}

#define TR(en, zh) (IsChinese() ? (zh) : (en))
