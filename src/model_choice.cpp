#include "model_choice.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace olas {

namespace {

constexpr const char *kFileName = "olas-model.txt";

// Mirrors the ARCH_* constants; kept local so this file has no engine
// dependency.
constexpr int ARCH_SMALL_STREAMING  = 4;
constexpr int ARCH_MEDIUM_STREAMING = 5;

std::string exe_dir() {
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string p(buf, n);
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

} // namespace

std::string model_choice_path() {
    return exe_dir() + "\\" + kFileName;
}

EnglishModel load_english_model() {
    FILE *f = std::fopen(model_choice_path().c_str(), "r");
    if (!f) return EnglishModel::Unset;

    char line[64] = {0};
    const char *got = std::fgets(line, sizeof line, f);
    std::fclose(f);
    if (!got) return EnglishModel::Unset;

    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    for (char &c : s) c = (char)tolower((unsigned char)c);

    if (s == "medium") return EnglishModel::Medium;
    if (s == "small")  return EnglishModel::Small;
    return EnglishModel::Unset;
}

bool save_english_model(EnglishModel m) {
    if (m == EnglishModel::Unset) return false;
    FILE *f = std::fopen(model_choice_path().c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "%s\n", english_model_name(m));
    std::fclose(f);
    return true;
}

const char *english_model_name(EnglishModel m) {
    switch (m) {
        case EnglishModel::Medium: return "medium";
        case EnglishModel::Small:  return "small";
        default:                   return "unset";
    }
}

int english_model_arch(EnglishModel m) {
    switch (m) {
        case EnglishModel::Medium: return ARCH_MEDIUM_STREAMING;
        case EnglishModel::Small:  return ARCH_SMALL_STREAMING;
        default:                   return 0;
    }
}

} // namespace olas
