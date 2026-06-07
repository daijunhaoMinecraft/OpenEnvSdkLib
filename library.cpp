#define WIN32_LEAN_AND_MEAN
#define PCRE2_CODE_UNIT_WIDTH 8

#include <windows.h>
#include <winsock2.h>
#include "library.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <pcre2.h>
#include <string>
#include <vector>
#include <thread>
#include <shared_mutex>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <MinHook.h>
#include <DbgHelp.h>
#include <psapi.h>

// Let the compiler know we use UTF-8
#pragma execution_character_set("utf-8")
#pragma comment(lib, "Dbghelp.lib")

#include <windows.h>
#include <winsock2.h>

const std::string MinecraftName = "Minecraft.Windows.exe";

// ======================= Log & Console System =======================
std::mutex g_log_mutex;
bool g_first_log = true;

void LogToConsoleAndFile(const std::string& level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_log_mutex);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "[" << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S") << "] [" << level << "] " << msg;
    std::string formatted = ss.str();

    std::cout << formatted << std::endl;
}

std::string GetStackTrace() {
    void* stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);

    if (frames == 0) return "No stack trace available.";

    std::string result;
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;

    for (USHORT i = 0; i < frames; i++) {
        DWORD64 address = (DWORD64)stack[i];

        if (SymFromAddr(GetCurrentProcess(), address, &displacement, pSymbol)) {
            result += std::to_string(i) + ": " + std::string(pSymbol->Name) +
                      " (Offset: 0x" + std::to_string(displacement) + ")\n";
        } else {
            result += std::to_string(i) + ": Unknown Symbol at 0x" +
                      std::to_string(address) + "\n";
        }
    }
    return result;
}

void SetupConsole() {
    if (AllocConsole()) {
        // Key step: Set console input/output encoding to UTF-8
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);

        FILE* fp;
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
        std::cout.clear();

        // Try changing font (default console font may not support UTF-8 display)
        CONSOLE_FONT_INFOEX cfi;
        cfi.cbSize = sizeof(cfi);
        cfi.nFont = 0;
        cfi.dwFontSize.X = 0;
        cfi.dwFontSize.Y = 16;
        cfi.FontFamily = FF_DONTCARE;
        cfi.FontWeight = FW_NORMAL;
        wcscpy_s(cfi.FaceName, L"SimSun-ExtB");
        SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &cfi);

        SetConsoleTitleA("Minecraft.Windows.exe Debug Monitor [UTF-8 Mode]");

        // Enable ANSI escape sequence support (Windows 10 1511+)
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);

        LogToConsoleAndFile("INFO", "Console opened and set to UTF-8");
    }
}

#define INFO_LOG(msg)  LogToConsoleAndFile("INFO", msg)
#define ERROR_LOG(msg) LogToConsoleAndFile("ERROR", msg)
#define DEBUG_LOG(msg) LogToConsoleAndFile("DEBUG", msg)

template<typename T>
std::string PtrToHex(T ptr) {
    std::stringstream ss; ss << "0x" << std::hex << std::uppercase << (uintptr_t)ptr; return ss.str();
}


// ======================= Game Log Capture (OutputDebugString Hook) =======================

// Wide string to UTF-8 conversion helper
std::string WideToUTF8(LPCWSTR wstr) {
    if (!wstr) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (size_needed <= 0) return "";
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &strTo[0], size_needed, NULL, NULL);
    strTo.resize(size_needed - 1); // remove trailing \0
    return strTo;
}

// Original function pointers
typedef VOID (WINAPI *tOutputDebugStringA)(LPCSTR);
typedef VOID (WINAPI *tOutputDebugStringW)(LPCWSTR);

tOutputDebugStringA pOriginalOutputDebugStringA = nullptr;
tOutputDebugStringW pOriginalOutputDebugStringW = nullptr;

// Hooked A version (narrow char)
VOID WINAPI HookedOutputDebugStringA(LPCSTR lpOutputString) {
    if (lpOutputString) {
        std::string msg = lpOutputString;
        // Strip trailing newlines to prevent double newlines from LogToConsoleAndFile
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();

        LogToConsoleAndFile("GAME_A", msg);
    }

    // Call original so downstream listeners are not affected
    if (pOriginalOutputDebugStringA) {
        pOriginalOutputDebugStringA(lpOutputString);
    }
}

// Hooked W version (wide char / Unicode)
VOID WINAPI HookedOutputDebugStringW(LPCWSTR lpOutputString) {
    if (lpOutputString) {
        std::string msg = WideToUTF8(lpOutputString);
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();

        LogToConsoleAndFile("GAME_W", msg);
    }

    if (pOriginalOutputDebugStringW) {
        pOriginalOutputDebugStringW(lpOutputString);
    }
}

// Hook installation
void SetupGameLogHooks() {
    if (MH_Initialize() != MH_OK) {
        ERROR_LOG("MinHook init failed, cannot capture game logs.");
        return;
    }

    MH_CreateHook(&OutputDebugStringA, &HookedOutputDebugStringA, reinterpret_cast<LPVOID*>(&pOriginalOutputDebugStringA));
    MH_CreateHook(&OutputDebugStringW, &HookedOutputDebugStringW, reinterpret_cast<LPVOID*>(&pOriginalOutputDebugStringW));

    if (MH_EnableHook(MH_ALL_HOOKS) == MH_OK) {
        INFO_LOG("Game debug log capture enabled (OutputDebugString Hooked)");
    } else {
        ERROR_LOG("Game log Hook enable failed!");
    }
}


// ======================= Core: Solve C2712 Error =======================

// A pure C function with no C++ objects, specifically for executing callbacks.
// This avoids the "cannot use __try in functions that require object unwinding" error.
__declspec(noinline) void RawExecuteCallback(CallbackFunc cb, int code, const char* res, void* ctx) {
    __try {
        if (cb) cb(code, res, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // If an exception occurs here, we can't call Log; just use raw OutputDebugString
        OutputDebugStringA("EnvSDK: CRASH CAUGHT IN CALLBACK!");
    }
}

// Thread-safe wrapper for callback execution
void ExecuteCallbackSafe(CallbackFunc cb, int code, const char* res, void* ctx) {
    if (!cb) {
        DEBUG_LOG("Callback skipped (null). Result: " + (res ? std::string(res) : "null"));
        return;
    }
    DEBUG_LOG("Executing Callback -> Code: " + std::to_string(code));

    // Call the raw executor (no C++ exception handling)
    RawExecuteCallback(cb, code, res, ctx);
}

// ======================= Memory Read/Write Safety =======================

__declspec(noinline) bool TryReadString(void* ptr, char* out, size_t maxLen) {
    __try {
        if (!ptr) return false;
        strncpy(out, (const char*)ptr, maxLen - 1);
        out[maxLen - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string SafeReadString(void* ptr) {
    char buf[2048] = {0};
    if (TryReadString(ptr, buf, sizeof(buf))) return std::string(buf);
    return "INVALID_PTR(" + PtrToHex(ptr) + ")";
}

__declspec(noinline) bool TryWriteBuffer(void* dest, const char* src, size_t len) {
    __try { memcpy(dest, src, len); ((char*)dest)[len] = '\0'; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SafeWriteBuffer(void* buf, size_t size, const std::string& data, const std::string& funcName) {
    if (!buf) { ERROR_LOG(funcName + " -> OutBuffer is NULL"); return; }
    size_t actualSize = size & 0xFFFFFFFF; // mask high-bit pollution
    size_t len = data.length();
    if (len >= actualSize) len = actualSize - 1;

    if (TryWriteBuffer(buf, data.c_str(), len)) {
        INFO_LOG(funcName + " -> Write Success to " + PtrToHex(buf) + ": " + data);
    } else {
        ERROR_LOG(funcName + " -> Access Violation writing to " + PtrToHex(buf));
    }
}

// ======================= Data Processing Logic =======================
struct Pcre2Regex { std::string RegexID; std::string Regex; pcre2_code_8* compiled_regex; };
struct RegexTypeStruct { std::string RegexType; std::vector<Pcre2Regex> RegexList; };
struct SensitiveWordFilter { std::vector<RegexTypeStruct> RegexList; };
struct DetectResult { bool isHit; int code; std::string message; std::string regularId; PCRE2_SIZE matchStart; PCRE2_SIZE matchEnd; };

std::shared_mutex g_filter_mutex;
SensitiveWordFilter g_filter;

// Base64 encode/decode
const std::string BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64_encode(const std::string& input) {
    std::string encoded; int val = 0, valb = -6;
    for (unsigned char c : input) { val = (val << 8) + c; valb += 8; while (valb >= 0) { encoded.push_back(BASE64_CHARS[(val >> valb) & 0x3F]); valb -= 6; } }
    if (valb > -6) encoded.push_back(BASE64_CHARS[((val << 8) >> (valb + 8)) & 0x3F]);
    while (encoded.size() % 4) encoded.push_back('='); return encoded;
}
std::vector<uint8_t> base64_decode_to_bytes(const std::string& input) {
    std::vector<uint8_t> decoded; int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break; size_t pos = BASE64_CHARS.find(c); if (pos == std::string::npos) continue;
        val = (val << 6) + pos; valb += 6; if (valb >= 0) { decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF)); valb -= 8; }
    } return decoded;
}

class RC4 {
    unsigned char s[256];
public:
    void init(const std::vector<uint8_t>& key) {
        int key_len = key.size(); unsigned char k[256];
        for (int i = 0; i < 256; i++) { s[i] = i; k[i] = key[i % key_len]; }
        int j = 0; for (int i = 0; i < 256; i++) { j = (j + s[i] + k[i]) % 256; std::swap(s[i], s[j]); }
    }
    std::vector<uint8_t> crypt(const std::vector<uint8_t>& data) {
        std::vector<uint8_t> result; result.reserve(data.size()); int i = 0, j = 0;
        for (size_t n = 0; n < data.size(); n++) { i = (i + 1) % 256; j = (j + s[i]) % 256; std::swap(s[i], s[j]); int t = (s[i] + s[j]) % 256; result.push_back(data[n] ^ s[t]); }
        return result;
    }
};

// ======================= Local Anti-Lag Rule Injection =======================
void InjectAntiLagRules(SensitiveWordFilter& filter) {
    RegexTypeStruct antiLagType;
    antiLagType.RegexType = "anti_lag";

    // Format: {"RuleID", "PCRE2 regex pattern"}
    std::vector<std::pair<std::string, std::string>> local_rules = {
        {"private_use_area", R"([\x{E000}-\x{F8FF}\x{F0000}-\x{FFFFF}\x{100000}-\x{10FFFF}])"}
    };
    INFO_LOG("Auto-injecting: filter unicode private use area");

    int count = 0;
    for (const auto& rule : local_rules) {
        int err;
        PCRE2_SIZE off;
        // PCRE2_UTF ensures \x{...} patterns match UTF-8 text
        pcre2_code_8* compiled = pcre2_compile_8(
            (PCRE2_SPTR8)rule.second.c_str(),
            PCRE2_ZERO_TERMINATED,
            PCRE2_UTF | PCRE2_UCP,
            &err,
            &off,
            NULL
        );

        if (compiled) {
            antiLagType.RegexList.push_back({rule.first, rule.second, compiled});
            count++;
        } else {
            ERROR_LOG("Failed to compile local rule: " + rule.first);
        }
    }

    if (count > 0) {
        filter.RegexList.push_back(antiLagType);
        INFO_LOG("Injected " + std::to_string(count) + " local anti-lag rules.");
    }
}

std::string GetSensitiveWordConfigOnline(const std::string& gameid, const std::string& secretKey, const std::string& host) {
    INFO_LOG("Requesting config. GameId=" + gameid + " SecretKey=" + secretKey + " host=" + host);
    std::string networkGameID = (gameid == "g79") ? "android_g79" : gameid;
    std::string request_data = "{\"info\":{\"deviceid\":\"1111-2222-3333-4444-5555\",\"gameid\":\"" + gameid + "\",\"network\":\"wifi\",\"sys\":\"cpp\",\"version\":\"1.0.9\"}}";

    httplib::Client cli(host);
    auto response1 = cli.Post("/initbox_" + networkGameID + ".html", base64_encode(request_data), "text/plain");

    if (!response1 || response1->status != 200) { ERROR_LOG("Network Step 1 Failed"); return ""; }

    try {
        nlohmann::json GetJsonResponse = nlohmann::json::parse(response1->body);
        std::string configUrl = GetJsonResponse["url"];
        DEBUG_LOG("Download URL: " + configUrl);

        size_t protocol_end = configUrl.find("://");
        std::string host = configUrl.substr(protocol_end + 3, configUrl.find('/', protocol_end + 3) - protocol_end - 3);
        std::string path = configUrl.substr(configUrl.find('/', protocol_end + 3));

        httplib::SSLClient ssl_cli(host.c_str());
        ssl_cli.enable_server_certificate_verification(false);
        auto response2 = ssl_cli.Get(path.c_str());

        if (!response2 || response2->status != 200) return "";
        std::vector<uint8_t> encrypted_data = base64_decode_to_bytes(response2->body);
        std::string key = secretKey;
        RC4 rc4; rc4.init(std::vector<uint8_t>(key.begin(), key.end()));
        std::vector<uint8_t> dec = rc4.crypt(encrypted_data);
        return std::string(dec.begin(), dec.end());
    } catch (...) { return ""; }
}

bool init_sensitive_word(SensitiveWordFilter& filter, const std::string& gameid, const std::string& secretKey, const std::string& host) {
    SetupGameLogHooks();
    std::string config = GetSensitiveWordConfigOnline(gameid, secretKey, host);
    if (config.empty()) return false;
    try {
        nlohmann::json json_content = nlohmann::json::parse(config);
        int count = 0;
        for (auto& item : json_content["regex"].items()) {
            RegexTypeStruct type; type.RegexType = item.key();
            for (auto& regex_info : item.value().items()) {
                int err; PCRE2_SIZE off;
                pcre2_code_8* compiled = pcre2_compile_8((PCRE2_SPTR8)regex_info.value().get<std::string>().c_str(), PCRE2_ZERO_TERMINATED, PCRE2_UTF | PCRE2_UCP, &err, &off, NULL);
                if (compiled) { type.RegexList.push_back({regex_info.key(), regex_info.value(), compiled}); count++; }
            }
            filter.RegexList.push_back(type);
        }
        INFO_LOG("Init success. Compiled patterns: " + std::to_string(count));
        # InjectAntiLagRules(filter);
        return true;
    } catch (...) { return false; }
}

// ANSI highlight: wrap matched range with red background + white text
std::string HighlightText(const std::string& text, PCRE2_SIZE start, PCRE2_SIZE end) {
    if (start >= end || start >= (PCRE2_SIZE)text.size()) return text;
    if (end > (PCRE2_SIZE)text.size()) end = (PCRE2_SIZE)text.size();
    return text.substr(0, start)
        + "\033[41;37m" + text.substr(start, end - start) + "\033[0m"
        + text.substr(end);
}

DetectResult CheckText(const std::string& text, const SensitiveWordFilter& filter, bool isNickname) {
    DetectResult res = {false, 200, "pass", "-1", 0, 0};
    for (const auto& regex_type : filter.RegexList) {
        if (isNickname != (regex_type.RegexType == "nickname")) continue;
        for (const auto& regex : regex_type.RegexList) {
            pcre2_match_data_8* match_data = pcre2_match_data_create_from_pattern_8(regex.compiled_regex, nullptr);
            int rc = pcre2_match_8(regex.compiled_regex, (PCRE2_SPTR)text.c_str(), (PCRE2_SIZE)text.size(), 0, 0, match_data, nullptr);
            if (rc > 0) {
                PCRE2_SIZE* ovector = pcre2_get_ovector_pointer_8(match_data);
                res.isHit = true; res.code = 201; res.message = regex_type.RegexType; res.regularId = regex.RegexID;
                res.matchStart = ovector[0]; res.matchEnd = ovector[1];
                DEBUG_LOG("Word Blocked: " + HighlightText(text, ovector[0], ovector[1]) + " (ID: " + regex.RegexID + ")");
                pcre2_match_data_free_8(match_data);
                return res;
            }
            pcre2_match_data_free_8(match_data);
        }
    }
    return res;
}

// ======================= Exported Interfaces =======================
extern "C" {
    __declspec(dllexport) void EnvSDK_clearSDK() {
        std::unique_lock<std::shared_mutex> lock(g_filter_mutex);
        for(auto& t : g_filter.RegexList) for(auto& r : t.RegexList) if(r.compiled_regex) pcre2_code_free_8(r.compiled_regex);
        g_filter.RegexList.clear();
        INFO_LOG("SDK Cleared.");
    }

    __declspec(dllexport) void EnvSDK_initSDK(const char* gameId, const char* secretKey, const char* host, CallbackFunc callback, void* context) {
        std::string gid = gameId ? gameId : "g79";
        bool success;
        { std::unique_lock<std::shared_mutex> lock(g_filter_mutex); success = init_sensitive_word(g_filter, gid, secretKey, host); }
        std::string res = success ? R"({"code":200,"message":"pass","regularId":"-1"})" : R"({"code":201,"message":"init error","regularId":"-1"})";
        ExecuteCallbackSafe(callback, success ? 200 : 201, res.c_str(), context);
    }

    __declspec(dllexport) void EnvSDK_reviewNickname(const char* nickname, CallbackFunc callback, void* context) {
        std::string nick = nickname ? nickname : "";
        DetectResult res;
        { std::shared_lock<std::shared_mutex> lock(g_filter_mutex); res = CheckText(nick, g_filter, true); }
        nlohmann::json j; j["code"] = res.code; j["message"] = res.message; j["regularId"] = res.regularId;
        ExecuteCallbackSafe(callback, res.code, j.dump().c_str(), context);
    }

    __declspec(dllexport) void EnvSDK_reviewWords(const char* content, const char* level, const char* channel, CallbackFunc callback, void* context) {
        std::string text = "level=" + std::string(level?level:"0") + "_content=" + std::string(content?content:"");
        DetectResult res;
        { std::shared_lock<std::shared_mutex> lock(g_filter_mutex); res = CheckText(text, g_filter, false); }
        nlohmann::json j; j["code"] = res.code; j["message"] = res.message; j["regularId"] = res.regularId;
        ExecuteCallbackSafe(callback, res.code, j.dump().c_str(), context);
    }

    // --- Simple Interface Group ---
    __declspec(dllexport) int EnvSDK_initSDKSimple(void* a1, void* a2, void* a3, void* a4, void* a5) {
        std::string gid = SafeReadString(a1);
        std::string secretKey = SafeReadString(a2);
        std::string host = SafeReadString(a3);

        INFO_LOG("SimpleInit Called. GameID=" + gid + " SecretKey=" + secretKey + " Host=" + host + " Buffer=" + PtrToHex(a4));
        bool success;
        { std::unique_lock<std::shared_mutex> lock(g_filter_mutex); success = init_sensitive_word(g_filter, gid.length() < 3 ? "g79" : gid, secretKey, host); }
        SafeWriteBuffer(a4, (size_t)a5, success ? R"({"code":200,"message":"pass","regularId":"-1"})" : R"({"code":201,"message":"error","regularId":"-1"})", "SimpleInit");
        return 100;
    }

    __declspec(dllexport) int EnvSDK_reviewNicknameSimple(void* a1, void* a2, void* a3, void* a4, void* a5) {
        std::string nick = SafeReadString(a1);
        DetectResult res;
        { std::shared_lock<std::shared_mutex> lock(g_filter_mutex); res = CheckText(nick, g_filter, true); }

        void* outBuf = nullptr; size_t outSize = 0;
        if ((uintptr_t)a3 > 0 && (uintptr_t)a3 < 0xFFFF) { outBuf = a2; outSize = (size_t)a3; }
        else { outBuf = a4; outSize = (size_t)a5; }

        nlohmann::json j; j["code"] = res.code; j["message"] = res.message; j["regularId"] = res.regularId;
        SafeWriteBuffer(outBuf, outSize, j.dump(), "SimpleNickname");
        return 100;
    }

    __declspec(dllexport) int EnvSDK_reviewWordsSimple(void* a1, void* a2, void* a3, void* a4, void* a5) {
        std::string content = SafeReadString(a1);
        DetectResult res;
        { std::shared_lock<std::shared_mutex> lock(g_filter_mutex); res = CheckText(content, g_filter, false); }
        INFO_LOG("[ReviewWords] Check Sensitive Words: \""
            + (res.isHit ? HighlightText(content, res.matchStart, res.matchEnd)
                         : ("\033[32m" + content + "\033[0m"))
            + "\" Check Result: "
            + (res.isHit ? "\033[31mBLOCKED\033[0m" : "\033[32mPASS\033[0m"));
        nlohmann::json j; j["code"] = res.code; j["message"] = res.message; j["regularId"] = res.regularId;

        SafeWriteBuffer(a4, (size_t)a5, j.dump(), "SimpleWords");
        return 100;
    }

    // Default wrappers
    __declspec(dllexport) void EnvSDK_initSDKAsync(const char* g, const char* s, const char* h, CallbackFunc c, void* ctx) { EnvSDK_initSDK(g,s,h,c,ctx); }
    __declspec(dllexport) void EnvSDK_reviewNicknameAsync(const char* n, CallbackFunc c, void* ctx) { EnvSDK_reviewNickname(n,c,ctx); }
    __declspec(dllexport) void EnvSDK_reviewWordsAsync(const char* c, const char* l, const char* ch, CallbackFunc cb, void* ctx) { EnvSDK_reviewWords(c,l,ch,cb,ctx); }
    __declspec(dllexport) int EnvSDK_setSwitch(const char* n, int v) { return 1; }
    __declspec(dllexport) int EnvSDK_getSwitch(const char* n, int* v) { if(v) *v=1; return 1; }
}

// ======================= DLL Main =======================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            SetupConsole();
            INFO_LOG("EnvSDK Lib Loaded Successfully.");
            break;

        case DLL_PROCESS_DETACH:
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            break;
    }
    return TRUE;
}
