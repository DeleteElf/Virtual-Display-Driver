#include <windows.h>
#include <iostream>
#include <string>
#include <map>
#include <thread>
#include <chrono>
#include <algorithm>
#include <sstream>
//必须使用这个命名空间，才能让 "ms" 后缀生效
using namespace std::chrono_literals;

struct IddCxVersionInfo {
    std::string windowsVersion;
    std::string iddcxVersion;
    ULONG versionValue;
};

std::map<DWORD, IddCxVersionInfo> versionMap = {
    {26100, {"Windows 11 24H2", "1.10", 0x1A80}},
    {22631, {"Windows 11 23H2", "1.10", 0x1A00}},
    {22621, {"Windows 11 22H2", "1.9", 0x1900}},
    {22000, {"Windows 11 21H2", "1.8", 0x1800}},
    {19045, {"Windows 10 22H2", "1.5", 0x1500}},
    {19044, {"Windows 10 21H2", "1.5", 0x1500}},
    {19043, {"Windows 10 21H1", "1.5", 0x1500}},
    {19042, {"Windows 10 20H2", "1.5", 0x1500}},
    {19041, {"Windows 10 20H1", "1.5", 0x1500}},
    {18363, {"Windows 10 19H2", "1.4", 0x1400}},
    {18362, {"Windows 10 19H1", "1.4", 0x1400}},
    {17763, {"Windows 10 RS5", "1.3", 0x1300}},
    {17134, {"Windows 10 RS4", "1.3", 0x1300}},
    {16299, {"Windows 10 RS3", "1.3", 0x1300}},
    {15063, {"Windows 10 Creators Update", "1.2", 0x1200}},
    {14393, {"Windows 10 Anniversary Update", "1.0", 0x1000}},
    {10240, {"Windows 10 RTM", "1.0", 0x1000}}
};

DWORD GetWindowsBuildNumber() {
    OSVERSIONINFOEX osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        
        if (RtlGetVersion) {
            RTL_OSVERSIONINFOW rovi = { 0 };
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (RtlGetVersion(&rovi) == 0) {
                return rovi.dwBuildNumber;
            }
        }
    }
    
    return 0;
}

IddCxVersionInfo GetIddCxVersionFromBuild(DWORD buildNumber) {
    auto it = versionMap.find(buildNumber);
    if (it != versionMap.end()) {
        return it->second;
    }
    
    for (auto& pair : versionMap) {
        if (buildNumber >= pair.first) {
            return pair.second;
        }
    }
    
    return {"Unknown Windows Version", "Unknown", 0x0000};
}
constexpr auto kMaxRetryCount = 3;
constexpr auto kInitialRetryDelay = 500ms;
constexpr auto kMaxRetryDelay = 5000ms;
const DWORD kPipeTimeoutMs = 5000;
const DWORD kPipeBufferSize = 4096;

std::wstring StringToWString(const std::string& str)
{
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstr(size_needed,0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], size_needed);
    return wstr;
}


std::chrono::milliseconds calculate_exponential_backoff(int attempt) {
    auto delay = kInitialRetryDelay * (1 << attempt);
    if(delay.count()>kMaxRetryDelay.count()){
        return kMaxRetryDelay;
    }
    return delay;
}

HANDLE connect_to_pipe_with_retry(const wchar_t *pipe_name, int max_retries=3) {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    int attempt = 0;
    auto retry_delay = kInitialRetryDelay;

    while (attempt < max_retries) {
        hPipe = CreateFileW(pipe_name,
                GENERIC_READ | GENERIC_WRITE,
                0,NULL,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,  // 使用异步IO
                NULL);

        if (hPipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (SetNamedPipeHandleState(hPipe, &mode, NULL, NULL)) {
                return hPipe;
            }
            CloseHandle(hPipe);
        }

        ++attempt;
        retry_delay = calculate_exponential_backoff(attempt);
        std::cout << "will retry after (ms):"<< retry_delay.count() <<std::endl;
        std::this_thread::sleep_for(retry_delay);
    }
    return INVALID_HANDLE_VALUE;
}

bool execute_pipe_command(const wchar_t *pipe_name, const wchar_t *command, std::string *response) {
    std::cout << "will exec pipe command:"<< command <<std::endl;
    auto hPipe = connect_to_pipe_with_retry(pipe_name);
    if (hPipe == INVALID_HANDLE_VALUE) {
//        std::cout << "连接MTT虚拟显示管道失败，已重试多次" <<std::endl;
        return false;
    }

    // 异步IO结构体
    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    struct HandleGuard {
        HANDLE handle;
        ~HandleGuard() {
            if (handle) CloseHandle(handle);
        }
    } event_guard { overlapped.hEvent };
    // 发送命令（使用宽字符版本）
    DWORD bytesWritten;
    size_t cmd_len = (wcslen(command) + 1) * sizeof(wchar_t);  // 包含终止符
    if (!WriteFile(hPipe, command, (DWORD) cmd_len, &bytesWritten, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) {
//            std::cout  << "发送" << command << std::endl;
            return false;
        }
        // 等待写入完成
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kPipeTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
//            std::cout  << "发送" << command << "命令超时\n";
            return false;
        }
    }

    // 读取响应
    if (response) {
        char buffer[kPipeBufferSize];
        DWORD bytesRead;
        if (!ReadFile(hPipe, buffer, sizeof(buffer), &bytesRead, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                std::cout  << "return error code: " << GetLastError()<<std::endl;
                return false;
            }

            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, kPipeTimeoutMs);
            if (waitResult == WAIT_OBJECT_0 && GetOverlappedResult(hPipe, &overlapped, &bytesRead, FALSE)) {
                buffer[bytesRead] = '\0';
                *response = std::string(buffer, bytesRead);
            }
        }
    }
    return true;
}


int main(int argc, char* argv[]) {
    if(argc==3) {
        /* 命令参考如下：
         IddCxVersionQuery \\.\pipe\CgTwVdd RELOAD_DRIVER
         */
        std::wstring pipe = StringToWString(argv[1]);
        std::wstring command = StringToWString(argv[2]);
//        std::wstring command = L"RELOAD_DRIVER";
//        const wchar_t *kVddPipeName = L"\\\\.\\pipe\\CgTwVdd";
        std::string result;
//        bool success = execute_pipe_command(kVddPipeName, command.c_str(), &result);
        bool success = execute_pipe_command(pipe.c_str(), command.c_str(), &result);
        std::cout << result << std::endl;
        if(success){
            std::cout << "exec command successful!\n";
        }else{
            std::cout << "exec command failed!\n";
        }
    }else {
        std::cout << "IddCx Version Query Tool\n";
        std::cout << "========================\n\n";

        DWORD buildNumber = GetWindowsBuildNumber();

        if (buildNumber == 0) {
            std::cout << "Error: Could not retrieve Windows build number.\n";
            return 1;
        }

        std::cout << "Windows Build Number: " << buildNumber << "\n";

        IddCxVersionInfo versionInfo = GetIddCxVersionFromBuild(buildNumber);

        std::cout << "Windows Version: " << versionInfo.windowsVersion << "\n";
        std::cout << "IddCx Version: " << versionInfo.iddcxVersion << "\n";
        std::cout << "IddCx Version Value: 0x" << std::hex << std::uppercase << versionInfo.versionValue << "\n\n";

        if (versionInfo.iddcxVersion == "Unknown") {
            std::cout << "Note: This Windows build may not support IddCx or uses an unknown version.\n";
            std::cout << "IddCx was introduced in Windows 10 Creators Update (build 15063).\n";
        } else {
            std::cout << "IddCx Framework Information:\n";
            std::cout << "- IddCx (Indirect Display Driver Class eXtension) is a framework for\n";
            std::cout << "  developing indirect display drivers in Windows.\n";
            std::cout << "- This version provides specific capabilities and APIs for indirect\n";
            std::cout << "  display driver development.\n";
        }
    }
    std::cout << "\nPress any key to exit...";
    std::cin.get();
    
    return 0;
}