#ifndef LIBRARY_H
#define LIBRARY_H

extern "C" {
    typedef void(*CallbackFunc)(int code, const char* result, void* context);

    __declspec(dllexport) void EnvSDK_clearSDK();
    __declspec(dllexport) int EnvSDK_setSwitch(const char* name, int value);
    __declspec(dllexport) int EnvSDK_getSwitch(const char* name, int* value);

    __declspec(dllexport) void EnvSDK_initSDK(const char* gameId, const char* secretKey, const char* host, CallbackFunc callback, void* context);
    __declspec(dllexport) void EnvSDK_initSDKAsync(const char* gameId, const char* secretKey, const char* host, CallbackFunc callback, void* context);
    __declspec(dllexport) void EnvSDK_reviewNickname(const char* nickname, CallbackFunc callback, void* context);
    __declspec(dllexport) void EnvSDK_reviewNicknameAsync(const char* nickname, CallbackFunc callback, void* context);
    __declspec(dllexport) void EnvSDK_reviewWords(const char* content, const char* level, const char* channel, CallbackFunc callback, void* context);
    __declspec(dllexport) void EnvSDK_reviewWordsAsync(const char* content, const char* level, const char* channel, CallbackFunc callback, void* context);

    __declspec(dllexport) int EnvSDK_initSDKSimple(void* a1, void* a2, void* a3, void* a4, void* a5);
    __declspec(dllexport) int EnvSDK_reviewNicknameSimple(void* a1, void* a2, void* a3, void* a4, void* a5);
    __declspec(dllexport) int EnvSDK_reviewWordsSimple(void* a1, void* a2, void* a3, void* a4, void* a5);
}

#endif // LIBRARY_H