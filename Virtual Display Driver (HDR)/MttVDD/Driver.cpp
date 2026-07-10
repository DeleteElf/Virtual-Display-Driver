/*++

Copyright (c) Microsoft Corporation

Abstract:

    This module contains a sample implementation of an indirect display driver. See the included README.md file and the
    various TODO blocks throughout this file and all accompanying files for information on building a production driver.

    MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

    User Mode, UMDF

--*/

#include "Driver.h"
#include "Driver.tmh"

#include "Utils.h"
#include "IOHelper.h"
#include "version.h"
#include <AdapterOption.h>

#include <minidumpapiset.h>
#include <cctype>
#include <mutex>

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

#define CONFIG_FILE "option.txt"
#define RESET_EVENT_NAME	"Global\\NztDisplayReset"
#define DISPLAY_SIGN_NAME	"Global\\NztDisplaySign"
#define DRIVER_NAME "NztVdd"
#define LOG_FILE_NAME DRIVER_NAME".log"
#define DUMP_FILE_NAME DRIVER_NAME".dmp"
#define EDID_FILE_NAME DRIVER_NAME"_edid.bin"

std::string configPath = "C:\\ProgramData\\Nzt\\Vdd";//默认配置路径 ，这边考虑到需要读取配置

struct{
    AdapterOption Adapter;
} Options;

struct  {
    HANDLE hPipe;
    IDDCX_ADAPTER adapter;//显示适配器
}PipeWorkingContext;


vector<DisplayInfo> displays; //显示器信息
vector<UINT> fpsList; //刷新率
vector<SIZE> res; //分辨率
vector<tuple<UINT,UINT,UINT>> displayResFpsList={{1920,1080,60},{1920,1080,60}}; //没有使用edid时，默认使用此配置

UINT displayCount = 1; //默认初始化一个虚拟显示器
int logLevel = 3; //默认初始化一个日志级别为信息级
CRITICAL_SECTION logSec;
string logFile;
string logsPath;
int64_t logMaxSize = 1024 * 1024;//默认1m，太大会打不开
bool useEdid = true; //默认使用edid
bool hardwareCursor = true;//默认使用硬件鼠标

const bool defaultDisplayRecordSteam = false; //是否启用流记录保存
const int defaultDisplayRecordSize = 100 * 1024 * 1024; //流记录保存的大小

typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(
    HANDLE hProcess,
    DWORD dwPid,
    HANDLE hFile,
    MINIDUMP_TYPE DumpType,
    CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
    );

LONG exception_handler(struct _EXCEPTION_POINTERS* apExceptionInfo)
{
    LOG_ERROR("dump");
    if (apExceptionInfo == nullptr || apExceptionInfo->ExceptionRecord == nullptr) {
        LOG_ERROR("Exception info is null.");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    PEXCEPTION_RECORD pRecord = apExceptionInfo->ExceptionRecord;   // 1. 获取异常记录
    // 2. 提取核心信息
    DWORD exceptionCode = pRecord->ExceptionCode;         // 错误码（如 0xC0000005 是内存访问违例）
    PVOID exceptionAddress = pRecord->ExceptionAddress;   // 崩溃发生的内存地址
    // 3. 打印到日志
    LOG_ERROR("Crash Detected! ExceptionCode: 0x%X, ExceptionAddress: %p",
              exceptionCode, exceptionAddress);
    // 如果是内存访问违例（Access Violation），还可以抓取是读冲突还是写冲突
    if (exceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2) {
        LOG_ERROR("Access Violation Details: %s address %p",
                  (pRecord->ExceptionInformation[0] ? "Write to" : "Read from"), pRecord->ExceptionInformation[1]);
    }
#ifdef UNICODE
    const auto mhLib = LoadLibrary(L"dbghelp.dll");
#else
    const auto mhLib = LoadLibrary("dbghelp.dll");
#endif // !UNICODE
    if (NULL == mhLib) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto pDump = reinterpret_cast<MINIDUMPWRITEDUMP>(GetProcAddress(mhLib, "MiniDumpWriteDump"));
    if (NULL == pDump) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto hFile = CreateFileA(
        IOHelper::pathCombine(configPath, DUMP_FILE_NAME).c_str(),
        GENERIC_WRITE,
        FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    const DWORD flags = MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
        MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData |
        MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo |
        MiniDumpWithFullAuxiliaryState | MiniDumpIgnoreInaccessibleMemory |
        MiniDumpWithTokenInformation;

    if (hFile != INVALID_HANDLE_VALUE)
    {
        _MINIDUMP_EXCEPTION_INFORMATION ExInfo;
        ExInfo.ThreadId = GetCurrentThreadId();
        ExInfo.ExceptionPointers = apExceptionInfo;
        ExInfo.ClientPointers = FALSE;

        pDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            hFile,
            (MINIDUMP_TYPE)flags,
            &ExInfo,
            nullptr,
            nullptr
        );
        CloseHandle(hFile);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

enum {
    STREAM_TYPE_HEAD = 1,
    STREAM_TYPE_CONFIG = 2,
    STREAM_TYPE_POS = 3,
    STREAM_TYPE_BUFFER = 4,
    STREAM_TYPE_LOG = 5,
};

const int SIGN_HEAD_LEN = 1024;
const int SIGN_INFO_CNT = 3;

const int STREAM_HEAD_LEN = 1024;
const int STREAM_CONFIG_LEN = 2048;
const int STREAM_POS_LEN = 1024;
const int FRAME_POS_CNT = 8;
const int STREAM_LOG_LEN = 4096;
const int STREAM_LOG_INFO_CNT = 30;

struct SignInfo {
    int64_t display_cnt;
    int64_t ts;
    uint8_t sign[32];
};

struct Sign {
    int32_t typ;
    int32_t len;

    int32_t sign_cnt;
    int32_t sign_len;
    int64_t sign_idx;

    SignInfo sign[3];
};

int64_t g_freq_per_us;
uint64_t getUs() {
    LARGE_INTEGER ticks;
    if (!QueryPerformanceCounter(&ticks))
    {
        return 0;
    }
    //auto a = ticks.QuadPart;
    //LOG_INFO("a=%lld, freq=%lld tt=%lld", a, g_freq_per_us, a / g_freq_per_us);
    return ticks.QuadPart / g_freq_per_us;
    //return GetTickCount64();
}

HANDLE initResetEvent(void** addr) {
    SECURITY_DESCRIPTOR secutityDese;
    InitializeSecurityDescriptor(&secutityDese, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&secutityDese, TRUE, NULL, FALSE);
    SECURITY_ATTRIBUTES securityAttr;
    securityAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    securityAttr.bInheritHandle = FALSE;
    securityAttr.lpSecurityDescriptor = &secutityDese;

    auto sign_name = DISPLAY_SIGN_NAME;
    auto sign_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, &securityAttr, PAGE_READWRITE | SEC_COMMIT, 0, SIGN_HEAD_LEN, sign_name);
    if (!sign_handle) {
        LOG_ERROR("create sign memory %s error: 0x%08x", sign_name, GetLastError());
        return NULL;
    }
    auto shmen_addr = MapViewOfFile(sign_handle, FILE_MAP_ALL_ACCESS, 0, 0, SIGN_HEAD_LEN);
    if (!shmen_addr) {
        LOG_ERROR("map sign memory %s error: 0x%08x", sign_name, GetLastError());
        return NULL;
    }
    memset(shmen_addr, 0, SIGN_HEAD_LEN);
    *addr = shmen_addr;

    auto sign = (Sign*)shmen_addr;
    sign->typ = 1;
    sign->len = SIGN_HEAD_LEN;
    sign->sign_cnt = SIGN_INFO_CNT;
    sign->sign_len = sizeof(SignInfo);

    return  CreateEventA(&securityAttr, false, false, RESET_EVENT_NAME);
}

void getSignKey(unsigned char key[32]) {
    unsigned char key1[8] = { '2', '0', '2', '6', '0', '7', '1', '0' };
    unsigned char key2[8] = { 'N', 'z', 'T', 'V', 'd', 'd', 'N', 'z' };
    unsigned char key3[8] = { 't', 'T', 'e', 'a', 'm', 'w', 'o', 'r' };
    unsigned char key4[8] = { 'k', '!', '%', '&', '*', '$', '@', '^' };
    for (int i = 0; i < 8; i++) {
        auto idx = i * 4;
        key[idx + 0] = key1[i];
        key[idx + 1] = key2[i];
        key[idx + 2] = key3[i];
        key[idx + 3] = key4[i];
    }
}

UINT getSignDisplayCount(void* addr) {
    auto sign = (Sign*)addr;
    auto idx = sign->sign_idx % SIGN_INFO_CNT;
    auto sign_info = sign->sign[idx];
    if (sign_info.display_cnt < 0) {
        return 0;
    }
    LOG_INFO("idx=%d cnt=%d, ts=%lld", idx, sign_info.display_cnt, sign_info.ts);

    auto ts = _time64(NULL);
    if (sign_info.ts < (ts - 60) || sign_info.ts >(ts + 60)) {
        //LOG_INFO("sign ts invalid");
        return 0;
    }

    return (UINT)sign_info.display_cnt;
}
#pragma region log
const char* debug = "DEBUG";
const char* info = "INFO";
const char* warn = "WARN";
const char* error = "ERROR";

void write_log(const char* level, const char* func, int line, const char* format, ...)
{
    if (level == debug && logLevel < (int)LogLevel::Debug)
        return;
    else if (level == info && logLevel < (int)LogLevel::Info)
        return;
    else if (level == warn && logLevel < (int)LogLevel::Warning)
        return;
    else if (level == error && logLevel < (int)LogLevel::Error)
        return;

    FILE* fp = NULL;
    EnterCriticalSection(&logSec);
    if (fopen_s(&fp, logFile.c_str(), "ab+, ccs=UTF-8") == 0 && fp != NULL) {
//    if (_wfopen_s(&fp, logFile.c_str(), L"ab+, ccs=UTF-8") == 0 && fp != NULL) {
        char msg[1024];
        va_list args;
        va_start(args, format);
        vsnprintf_s(msg, sizeof(msg) - 1, format, args);
        va_end(args);

        // 获取当前时间点
        auto currentTime = std::chrono::system_clock::now();
        // 2. 将时间点转换为 time_t（精确到秒）
        std::time_t now = std::chrono::system_clock::to_time_t(currentTime);
        struct tm t;
        localtime_s(&t, &now);

        auto ts = currentTime.time_since_epoch();
        auto ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(ts).count() % 1000);
        fprintf_s(fp, "[%04d-%02d-%02d %02d:%02d:%02d,%03d][%s][proId:%d][threadId:%d][func:%s][line:%d]: %s\r\n",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ms,
            level, GetCurrentProcessId(), GetCurrentThreadId(), func, line, msg);
        auto file_len = ftell(fp);
        fclose(fp);
        if (logMaxSize > 0 && logMaxSize <= file_len) {
            IOHelper::backupFile(logFile, logsPath);
//            IOHelper::backupFile(WStringToString(logFile), logsPath);
        }
    }
    LeaveCriticalSection(&logSec);
}

#pragma endregion

#pragma region init
/*
// 1. 定义一个标准的 128 字节基础 EDID 模板（这里以一个通用 1080p 显示器为例）
// 注意：第 127 字节是校验和，我们在代码里动态计算，这里先填 0x00
unsigned char g_BaseEdidTemplate[128] = {
0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, // 0-7: 固定的 EDID 头部
0x4C, 0x2D,                                     // 8-9: 厂商代码 (样例: SEC)
0x00, 0x00,                                     // 10-11: 产品代码 (将在代码中动态修改)
0x00, 0x00, 0x00, 0x00,                         // 12-15: 32位序列号 (将在代码中动态修改)
0x01, 0x1E,                                     // 16-17: 制造周、年份
0x01, 0x04,                                     // 18-19: EDID 版本版本号 (1.4)
0xA5, 0x34, 0x20, 0x78, 0x22, 0xEE, 0x95, 0xA3, // 20-27: 图像显示参数与特性
0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54, 0xA1, // 28-35: 颜色特征与基础时序
0x08, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 36-43: 标准时序识别码
0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 44-51: 标准时序识别码
0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, // 52-59: 详细时序描述符 (1920x1080 60Hz)
0x58, 0x2C, 0x45, 0x00, 0xDD, 0x0C, 0x11, 0x00, // 60-67:
0x00, 0x1E, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x32, // 68-75: 显示器范围限制
0x3C, 0x1F, 0x50, 0x11, 0x00, 0x0A, 0x20, 0x20, // 76-83:
0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC, // 84-91: 显示器名称描述符
0x00, 0x56, 0x69, 0x72, 0x74, 0x75, 0x61, 0x6C, // 92-99:  "Virtual"
0x44, 0x69, 0x73, 0x70, 0x0A, 0x20, 0x00, 0x00, // 100-107: "Disp"
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 108-115: 空白描述符
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 116-123:
0x00, 0x00, 0x00,                               // 124-126:
0x00                                            // 127: 校验和占位符 (将在代码中动态计算 🔴)
};
*/
const BYTE defaultEdid[256] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, // 0-7: 固定的 EDID 头部
        0x0C, 0x64,                                     // 8-9: 厂商代码 (样例: SEC)
        0x37, 0x1A,                                     // 10-11: 产品代码 (将在代码中动态修改)
        0x15, 0xcd, 0x5b, 0x07,                         // 12-15: 32位序列号 (将在代码中动态修改)
        0x18, 0x1A,                                     // 16-17: 制造周、年份
        0x01, 0x04,                                     // 18-19: EDID 版本版本号 (1.4)
        0xb2, 0x3c, 0x22, 0x78, 0x04, 0xe6, 0xc5, 0xad, // 20-27: 图像显示参数与特性
        0x4e, 0x43, 0xaa, 0x26, 0x0b, 0x50, 0x54, 0x01, // 28-35: 颜色特征与基础时序
        0x40, 0x00, 0xd1, 0xc0, 0x95, 0xc0, 0xd1, 0xcf, // 36-43: 标准时序识别码
        0x95, 0xcf, 0xd1, 0xde, 0x95, 0xde, 0xd1, 0xfc, // 44-51: 标准时序识别码
        0x95, 0xfc, 0x4d, 0xd0, 0x00, 0xa0, 0xf0, 0x70, // 52-59: 详细时序描述符 (1920x1080 60Hz)
        0x3e, 0x80, 0x30, 0x20, 0x35, 0x00, 0x80, 0x88, // 60-67:
        0x42, 0x00, 0x00, 0x18, 0x5a, 0x87, 0x80, 0xa0, // 68-75: 显示器范围限制
        0x70, 0x38, 0x4d, 0x40, 0x30, 0x20, 0x35, 0x00, // 76-83:
        0x40, 0x44, 0x21, 0x00, 0x00, 0x18, 0x09, 0xec, // 84-91: 显示器名称描述符
        0x00, 0xa0, 0xa0, 0xa0, 0x67, 0x50, 0x30, 0x20, // 92-99:  "Virtual"
        0x35, 0x00, 0x00, 0xb0, 0x31, 0x00, 0x00, 0x18, // 100-107: "Disp"
        0x00, 0x00, 0x00, 0xfc, 0x00, 0x43, 'g', 'T', // 108-115: 空白描述符
        'w', 'V', 'd', 'd', 0x0a, 0x20, 0x20, 0x20, // 116-123:
        0x20, 0x20, 0x00,
        0x00,
};

//void modifyEdid(char* edid,UINT displayIndex) {
//    edid[8] = 0x0C;
//    edid[9] = 0x64;
//    edid[10] = 0x37+displayIndex ;//product_id 低位
//    edid[11] = 0x1A; //product_id 高位
//}

BYTE calculateChecksum(const char* edid) {
    int sum = 0;
    for (int i = 0; i < 127; ++i) {
        sum += edid[i];
    }
    sum %= 256;
    if (sum != 0) {
        sum = 256 - sum;
    }
    return static_cast<BYTE>(sum);
}

/// <summary>
/// 默认分辨率
/// </summary>
const SIZE defaultDisplayResolution[] =
{
        // 8k
        //{ 7680, 5760 }, //  4:3  WHUSXGA
        //{ 7680, 4800 }, // 16:10 WHUXGA
        //{ 7680, 4320 }, // 16:9  8K UHD
        // 5k
        //{ 5120, 4096 }, //  5:4  HSXGA
        //{ 5120, 3840 }, //  4:3  WHSXGA
        //{ 5120, 3200 }, // 16:10 WHXGA
        //{ 5120, 2880 }, // 16:9  UHD++
        //{ 5120, 2160 }, // 21:9  5K UHD
        // 4k
        //{ 4096, 3072 }, //  4:3  HXGA
        //{ 4096, 2160 }, // 19:10 DCI 4K
        //{ 3840, 2880 }, //  4:3  WQUSXGA
        //{ 3840, 2400 }, // 16:10 WQUXGA
        { 3840, 2160 }, // 16:9  4K UHD
//        { 3840, 1600 }, // 12:5  UW4K
        // 2.5k
        //{ 3000, 2000 }, //  3:2          : matebook x pro
        //{ 2560, 2048 }, //  5:4  USXGA
//        { 2560, 1600 }, // 16:10 UWXGA
        { 2560, 1440 }, // 16:9  QHD/WQHD: retina macbook 13 pro
//        { 2560, 1080 }, // 21:9  CINEMA 21:9
        //{ 2160, 1440 }, //  3:2          : matebook 13/14
        // 2k
//        { 2048, 1536 }, //  4:3   SUVGA(QXGA)
//        { 2048, 1280 }, // 16:10 DCI 2K
//        { 2048, 1152 }, // 16:9   QWXGA
//        { 1920, 1440 }, //  4:3  WUSXGA
//        { 1920, 1200 }, // 16:10  WUXGA
        { 1920, 1080 }, // 16:9  WSUVGA+(WSUGA/HDTV)
        // 1600
//        { 1680, 1050 }, // 16:10 WSXGA+
//        { 1600, 1200 }, //  4:3  USVGA/UXGA/UGA
//        { 1600, 1024 }, // 25:16 WSXGA
//        { 1600,  900 }, // 16:9  HD+
        // 1440
//        { 1440,  960 }, //  3:2  FWXGA+
//        { 1440,  900 }, // 16:10  WXGA+  : macbook 13 air
        { 1366,  768 }, // 16:9  FWXGA
        // 1280
//        { 1280, 1024 }, //  5:4   SXGA
//        { 1280,  960 }, //  4:3   SXGA-/UVGA
//        { 1280,  800 }, // 16:10  WXGA
//        { 1280,  768 }, // 15:9   WXGA
        { 1280,  720 }, // 16:9   WXGA/HD
        { 1024,  768 }, //  4:3    XGA
        {  800,  600 }  //  4:3   SVGA
};


/// <summary>
/// 默认的显示刷新率
/// </summary>
const UINT defaultDisplayFps[] =
{
    60
};

bool parseSizePerLine(const char* line, SIZE& size)
{
    size.cx = atoi(line);
    if (size.cx == 0) {
        return false;
    }
    line = strchr(line, 'x');
    if (line == NULL) {
        return false;
    }
    line++;
    size.cy = atoi(line);
    if (size.cy == 0) {
        return false;
    }
    return true;
}

void init() {
    InitializeCriticalSection(&logSec);
    int level=readFromRegWord(L"level");
    if (level != -1)
        logLevel = level;
    auto path = readFromRegString(L"path");
    if (!path.empty()) {
        IOHelper::checkAndCreateDirectory(configPath);//创建旧路径，可以用于甄别驱动是否进入加载逻辑
        configPath = WStringToString(path);
        LOG_DEBUG("从注册表配置读取到新的配置路径===>%s", configPath.c_str());
        if (IOHelper::checkAndCreateDirectory(path)) {//如果目录不存在，则创建目录
            LOG_DEBUG("新路径创建成功！");
            logFile = IOHelper::pathCombine(configPath, LOG_FILE_NAME);
        }
    }
    IOHelper::checkAndCreateDirectory(configPath);//如果目录不存在，则创建目录
    //logFile = StringToWString( IOHelper::pathCombine(configPath, logFilename));
    logFile = IOHelper::pathCombine(configPath, LOG_FILE_NAME);
    LOG_DEBUG("日志创建成功！");

    logsPath = IOHelper::pathCombine(configPath, "logs");
    IOHelper::checkAndCreateDirectory(logsPath);//生成日志的备份目录
    LOG_DEBUG("日志备份目录创建成功！");

    //todo:因为好几个地方都需要按这个最大数初始化，在没有明确新方案前，暂时不提供修改最大连接数的支持
//    auto maxCount = readFromRegWord(L"MaxCount");
//    if (maxCount > 0)
//        monitorMaxCount = maxCount;
}

void initDisplay(UINT displayIndex, DisplayInfo& display)
{
    LOG_INFO("正在初始化第[%d]个虚拟显示器,useEdid:%s",displayIndex,useEdid?"true":"false");
    display.use_edid = useEdid;
    display.hw_cursor = hardwareCursor;
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step0", displayIndex);
    if(displayResFpsList.size()<=displayIndex){ //手动配置的配置可能存在问题，这边进行自动修正。
        int index = (int)displayResFpsList.size() - 1;
        if (index < 0)
            index = 0;
        displayIndex=(UINT)index;
        LOG_WARN("检测到当前显示器索引[%d]，实际配置数量[%d],默认采用最后一个有效配置",displayIndex,displayResFpsList.size())
    }
    auto config=displayResFpsList[displayIndex];
    display.prefer_width =  get<0>(config);// res[0].cx;
    display.prefer_height =get<1>(config);// res[0].cy;
    display.prefer_fps = get<2>(config);//fpsList[0];
    //todo:流保存的逻辑，主要用于测试
    display.record_stream =defaultDisplayRecordSteam; // GetPrivateProfileIntA(INI_DISPLAY, INI_DISPLAY_RECORD_STREAM, kDisplayRecordSteam, ini_path);
    display.record_size =defaultDisplayRecordSize;// GetPrivateProfileIntA(INI_DISPLAY, INI_DISPLAY_RECORD_SIZE, kDisplayRecordSize, ini_path);
    if (display.record_stream)//暂时不启用这个
    {
        string format = IOHelper::pathCombine(configPath, "display%d.stream");
        snprintf(display.record_path, sizeof(display.record_path) - 1, format.c_str(), displayIndex + 1);
    }
    //todo:读取edid的逻辑，主要用于自定义显示器
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step1", displayIndex);
    display.edid_len = 0;
    string edidFile=IOHelper::pathCombine(configPath,EDID_FILE_NAME);
    if (IOHelper::exists(edidFile)) {//如果存在外部配置
        vector<char> vec;
        if (IOHelper::readEdIdData(edidFile.c_str(), vec)) {
            if (vec.size() == 128 || vec.size() == 256) {
                memcpy(display.edid_data, &vec[0], vec.size());
                display.edid_len = vec.size() / sizeof(display.edid_data[0]);
                LOG_INFO("%s use %s edid: count=%d", configPath.c_str(), EDID_FILE_NAME, display.edid_len);
            }
            else {
                LOG_ERROR("%s read %s length=%d", configPath.c_str(), EDID_FILE_NAME, vec.size());
            }
        }
    }
    // 读取不到则使用默认配置
    if (display.edid_len == 0) {
        display.edid_len = _countof(defaultEdid);
        memcpy(display.edid_data, defaultEdid, sizeof(display.edid_data[0]) * display.edid_len);
        LOG_INFO("使用默认的edid: %d", display.edid_len);
    }
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step2", displayIndex);
    // edid 的厂商改成CPY，产品id的最后一位改成对应的驱动实例，
    // 这样屏幕的ID类似 MONITOR\CPYD0C0\{4d36e96e-e325-11ce-bfc1-08002be10318}\0050
//    auto new_product_id = (display.edid_data[0x0A] & 0xF0) + displayIndex;
//    auto check_sum = (int)display.edid_data[0x7F];
//    check_sum += (display.edid_data[0x08] - 0x0E);
//    check_sum += (display.edid_data[0x09] - 0x19);
//    check_sum += (display.edid_data[0x0A] - new_product_id);
//    display.edid_data[0x08] = 0x0E;
//    display.edid_data[0x09] = 0x19;
//    display.edid_data[0x0A] = (BYTE)new_product_id;
//    display.edid_data[0x7F] = (BYTE)check_sum;
//    modifyEdid(reinterpret_cast<char*>(display.edid_data),displayIndex);
    display.edid_data[10] =(BYTE) (55 + displayIndex); //0x37+displayIndex
    LOG_INFO("使用默认的edid 的 product_id: %d", display.edid_data[10]*256+display.edid_data[11]);
    display.edid_data[127]= calculateChecksum(reinterpret_cast<char*>(display.edid_data));
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step3", displayIndex);
    display.size_cnt = res.size();//最多256个
    for (int i = 0; i < res.size(); i++)
        display.size_list[i] = res[i];
    //LOG_INFO("正在初始化第[%d]个虚拟显示器,step4", displayIndex);
    display.fps_cnt = fpsList.size();//最多16个
    std::copy(fpsList.begin(), fpsList.end(), display.fps_list);
//    for(int i=0;i<display.fps_cnt;i++){
//        LOG_DEBUG("支持的FPS：%d", display.fps_list[i]);
//    }
}

void initDisplays() {
    LOG_INFO("显示配置初始化,虚拟显示器数量:%d",displayCount);
    displays.clear();
    for (UINT i = 0; i < displayCount; i++)
    {
        displays.push_back(DisplayInfo());//初始化
        initDisplay(i, displays[i]);
    }
    LOG_INFO("显示配置初始化完成！");
}


void loadSettings() {
    auto configFile = IOHelper::pathCombine(configPath, CONFIG_FILE);
    auto configs = IOHelper::readAllLine(configFile);
    LOG_INFO("开始加载配置，读取到的配置条数:%d",configs.size());
    res.clear();//清空数据
    fpsList.clear();//清空数据
    hardwareCursor = true;//默认为true
    displayCount = 1;//默认一个
    if (configs.size() > 0) {
        for (int i = 0; i < configs.size(); i++)
        {
            /* 分析配置的主要逻辑
              * 1.如果存在注释符，注释之后，都不要
              * 2.如果存在=，则说明是指定属性
              * 3.如果不存在=，则说明是可用分辨率，可用分辨率用,号隔开
              * 4.如果存在多级数组，则第二级使用;号隔开
              */
              //parseSetting(config[i]);
            auto data = trimString(configs[i]);
            //string content = "正在处理配置0:" + data;
            //LOG_DEBUG(content.c_str());
            auto index = data.find_first_of('#');
            //LOG_DEBUG("正在处理配置注释:%d",index);
            if (index != 0) {//如果是0，说明整行配置都是注释，直接跳过此行
                if (index != std::string::npos) { //大于0，说明存在注释
                    //LOG_DEBUG("正在处理注释！");
                    data = trimString(data.erase(index));//去掉#之后的数据
                }
                //LOG_DEBUG("正在处理配置2");
                if (!data.empty()) {
                    string content1 = "正在处理配置:" + data;
                    LOG_DEBUG(content1.c_str());
                    index = data.find_first_of('=');
                    if (index != std::string::npos) { //属性配置
                        if (index > 0) {
                            auto setting = split(data, '=');
                            auto key = trimString(setting[0]);
                            if (key == "DisplayCount") {
                                auto count = stoi(setting[1]);
                                if (count >= 0) { //允许配置0个虚拟显示器
                                    displayCount = count;
                                }
                                if (count > monitorMaxCount) {
                                    displayCount = monitorMaxCount;
                                }
                            }
                            else if (key == "Displays") {
                                auto displaySetting = trimString(setting[1]);
                                if (!displaySetting.empty()) {
                                    displayResFpsList.clear();
                                    vector<string> strvec = split(setting[1], ';');
                                    for (int j = 0; j < strvec.size(); j++) {
                                        vector<string> temp = split(strvec[j], ',');
                                        if (temp.size() == 3) {
                                            displayResFpsList.push_back(make_tuple(stoi(temp[0]), stoi(temp[1]), stoi(temp[2])));
                                        }
                                    }
                                }
                            }
                            else if (key == "Rates") {
                                vector<string> strvec = split(setting[1], ',');
                                for (int j = 0; j < strvec.size(); j++)
                                {
                                    fpsList.push_back(stoi(strvec[j]));
                                }
                            }
                            else if (key == "LogLevel") {
                                auto option = trimString(setting[1]);
                                option = toLowerString(option);
                                if (option == "debug" || option == "4") {
                                    logLevel = 4;
                                }
                                else if (option == "info" || option == "3") {
                                    logLevel = 3;
                                }
                                else if (option == "warn" || option == "2") {
                                    logLevel = 2;
                                }
                                else if (option == "error" || option == "1") {
                                    logLevel = 1;
                                }
                                else {
                                    logLevel = 0;
                                }
                            }
                            else if (key == "HardwareCursor") {
                                auto option = trimString(setting[1]);
                                option = toLowerString(option);
                                hardwareCursor = option == "true";
                            }
                            else if (key == "UseEdid") {
                                auto option = trimString(setting[1]);
                                option = toLowerString(option);
                                useEdid = option == "true";
                            }
                        }
                    }else { //分辨率配置
                        vector<string> strvec = split(data, ',');
                        if (strvec.size() == 2) {
                            res.push_back({ stoi(strvec[0]), stoi(strvec[1]) });
                        }
                    }
                }
            }
        }
        LOG_INFO("使用option.txt配置的内容！");
    }else {
        LOG_INFO("使用默认配置的内容！");
    }
    if (fpsList.empty()) {//加入默认
        LOG_INFO("使用默认配置的刷新率！");
        fpsList.insert(fpsList.end(), begin(defaultDisplayFps),end(defaultDisplayFps));
    }else{
        LOG_INFO("使用自定义的刷新率配置，共%d条！",fpsList.size());
    }
    if (res.empty()) {
        LOG_INFO("使用默认配置的分辨率！");
        res.insert(res.end(), begin(defaultDisplayResolution), end(defaultDisplayResolution));
    }else{
        LOG_INFO("使用自定义的分辨率配置，共%d条！",res.size());
    }
    initDisplays();
}

#pragma endregion

#pragma region SampleMonitors
// Default modes reported for edid-less monitors. The first mode is set as preferred
//static const struct IndirectSampleMonitor::SampleMonitorMode s_SampleDefaultModes[] =
//{
//        { 1920, 1080, 60 },
//        { 1600,  900, 60 },
//        { 1024,  768, 75 },
//};
//
//// FOR SAMPLE PURPOSES ONLY, Static info about monitors that will be reported to OS
//static const struct IndirectSampleMonitor s_SampleMonitors[] =
//        {
//                // Modified EDID from Dell S2719DGF
//                {
//                        {
//                                0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x10,0xAC,0xE6,0xD0,0x55,0x5A,0x4A,0x30,0x24,0x1D,0x01,
//                                0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,0x0B,0x50,0x54,0x00,0x02,0x00,
//                                0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,
//                                0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,
//                                0x00,0x37,0x4A,0x51,0x58,0x42,0x59,0x32,0x0A,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,
//                                0x53,0x32,0x37,0x31,0x39,0x44,0x47,0x46,0x0A,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFD,0x00,0x28,
//                                0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x2C
//                        },
//                        {
//                                { 2560, 1440, 144 },
//                                { 1920, 1080,  60 },
//                                { 1024,  768,  60 },
//                        },
//                        0
//                },
//                // Modified EDID from Lenovo Y27fA
//                {
//                        {
//                                0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x30,0xAE,0xBF,0x65,0x01,0x01,0x01,0x01,0x20,0x1A,0x01,
//                                0x04,0xA5,0x3C,0x22,0x78,0x3B,0xEE,0xD1,0xA5,0x55,0x48,0x9B,0x26,0x12,0x50,0x54,0x00,0x08,0x00,
//                                0xA9,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x68,0xD8,0x00,
//                                0x18,0xF1,0x70,0x2D,0x80,0x58,0x2C,0x45,0x00,0x53,0x50,0x21,0x00,0x00,0x1E,0x00,0x00,0x00,0x10,
//                                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFD,0x00,
//                                0x30,0x92,0xB4,0xB4,0x22,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,0x4C,
//                                0x45,0x4E,0x20,0x59,0x32,0x37,0x66,0x41,0x0A,0x20,0x20,0x20,0x00,0x11
//                        },
//                        {
//                                { 3840, 2160,  60 },
//                                { 1600,  900,  60 },
//                                { 1024,  768,  60 },
//                        },
//                        0
//                }
//        };

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64)VSync) * ((UINT64)Width) * ((UINT64)Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD VirtualDisplayDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY VirtualDisplayDeviceD0Entry;

//EVT_IDD_CX_DEVICE_IO_CONTROL   VirtualDisplayIoDeviceControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED VirtualDisplayAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES VirtualDisplayAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION VirtualDisplayParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES VirtualDisplayMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES VirtualDisplayMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN VirtualDisplayMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN VirtualDisplayMonitorUnassignSwapChain;
EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP VirtualDisplayMonitorSetGammaRamp;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        if (pContext) {
            delete pContext;
            pContext = nullptr;
        }
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        if (pContext) {
            delete pContext;
            pContext = nullptr;
        }
    }
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

UINT g_CurConnectorIndex = 0;

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE hInstance,_In_ UINT dwReason,_In_opt_ LPVOID lpReserved)
{
    //LOG_INFO("");
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

//_Use_decl_annotations_
//extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT  pDriverObject,PUNICODE_STRING pRegistryPath)
//{
//    SetUnhandledExceptionFilter(exception_handler);
//    init();
//    LOG_INFO("call WdfDriverCreate");
//
//    WDF_DRIVER_CONFIG Config;
//    NTSTATUS Status;
//
//    WDF_OBJECT_ATTRIBUTES Attributes;
//    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
//
//    WDF_DRIVER_CONFIG_INIT(&Config,VirtualDisplayDeviceAdd);
//
//    Config.EvtDriverUnload = EvtDriverUnload;
//
//    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
//    if (!NT_SUCCESS(Status))
//    {
//        LOG_ERROR("WdfDriverCreate error: 0x%08x", Status);
//        return Status;
//    }
//
//    return Status;
//}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
//    UNREFERENCED_PARAMETER(Device);
    //UNREFERENCED_PARAMETER(PreviousState);
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    if (NULL == pContext || NULL == pContext->pContext) {
        LOG_ERROR("Get DeviceObject=0x%p NULL", Device);
        return STATUS_INVALID_PARAMETER;
    }

    // This function is called by WDF to start the device in the fully-on power state.

    LOG_INFO("call pContext->InitAdapter start, PreviousState=%d", PreviousState);
    pContext->pContext->InitAdapter();
    LOG_INFO("call pContext->InitAdapter end");
    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{

}

Direct3DDevice::Direct3DDevice()
{
    AdapterLuid = LUID{};
}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        LOG_ERROR("CreateDXGIFactory2 error: 0x%08x", hr);
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        LOG_ERROR("DxgiFactory->EnumAdapterByLuid error: 0x%08x", hr);
        return hr;
    }

    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if (FAILED(hr))
    {
        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        LOG_ERROR("D3D11CreateDevice error: 0x%08x", hr);
        return hr;
    }

    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IndirectMonitorContext* monitorContext, const InitChainParam& param, IDDCX_SWAPCHAIN hSwapChain,
                                       shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
        : m_monitorContext(monitorContext),m_InitParam(param), m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
    // Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    LOG_INFO("display[%d] SetEvent Terminate", m_InitParam.nConnectorIndex);
    // Alert the swap-chain processing thread to terminate
    isClosed=true;
    if (m_hThread.Get())
    {
        // Wait for the thread to terminate
        WaitForSingleObject(m_hThread.Get(), INFINITE);
        LOG_INFO("display[%d] Terminate success", m_InitParam.nConnectorIndex);
    }
    m_monitorContext=nullptr;
}

void SwapChainProcessor::ClearEncoder()
{
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
    // prioritize this thread for improved throughput in high CPU-load scenarios.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);
    LOG_INFO("display[%d] RunCore start", m_InitParam.nConnectorIndex);
    RunCore();
    LOG_INFO("display[%d] RunCore end", m_InitParam.nConnectorIndex);
    ClearEncoder();

    // Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
    // provide a new swap-chain if necessary.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;

    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
    DWORD currentRetryDelay = 0;
    const DWORD maxRetryDelay = 1000;//1秒内重试
    const DWORD retryDelay=10;//10毫秒一次
    int retryCount = 0;
    const int maxRetries = 100;//最多100次

    // Get the DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        LOG_ERROR("display[%d] 转成dxgi设备失败。。。",m_InitParam.nConnectorIndex)
        return;
    }
    DXGI_ADAPTER_DESC1 desc;
    m_Device->Adapter->GetDesc1(&desc);
    LOG_INFO("display[%d] Adapter desc: %S Luid:%04x-%04x VendorId=%04x DeviceId=%04x SubSysId=%04x"
            , m_InitParam.nConnectorIndex,
             desc.Description, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
             desc.VendorId, desc.DeviceId, desc.SubSysId);

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        switch (hr)
        {
            case DXGI_ERROR_ACCESS_LOST:
                LOG_INFO("display[%d] IddCxSwapChainSetDevice return DXGI_ERROR_ACCESS_LOST", m_InitParam.nConnectorIndex);
                break;
            default:
                LOG_ERROR("display[%d] IddCxSwapChainSetDevice error: 0x%08x", m_InitParam.nConnectorIndex, hr);
                break;
        }
        return;
    }
    LOG_INFO("display[%d] IddCxSwapChainSetDevice suc", m_InitParam.nConnectorIndex);
    if (displays[m_InitParam.nConnectorIndex].hw_cursor) {
        m_monitorContext->SetHwCursorMode();/* 设置硬件鼠标模式 */
    }
    for (;;)
    {
        // Ask for the next buffer from the producer
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        // AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
        if (hr == E_PENDING)
        {
            HANDLE waitHandles[2] = {};
            DWORD waitHandleCount = 0;
            if (m_hAvailableBufferEvent != nullptr && m_hAvailableBufferEvent != INVALID_HANDLE_VALUE){
                waitHandles[waitHandleCount++] = m_hAvailableBufferEvent;
            }
            if (waitHandleCount == 0){
                LOG_ERROR("No valid wait handles available while waiting for the next frame.");
                break;
            }
            DWORD waitResult = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, INFINITE);
            if(isClosed) {
                LOG_INFO("Terminate event signaled. Exiting loop.");
                break;
            }
            if (waitResult == WAIT_OBJECT_0){ //简单条件要置于复杂条件后面
                continue;
            } else {
                hr = HRESULT_FROM_WIN32(waitResult == WAIT_FAILED ? GetLastError() : waitResult);
                LOG_ERROR("Unexpected wait result. HRESULT:0x%08X", hr);
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
//            LOG_INFO("display[%d] Swap Chain Success!", m_InitParam.nConnectorIndex);
            IDXGIResource* pSurface = Buffer.MetaData.pSurface;
            currentRetryDelay = 0;
            retryCount = 0;
            ComPtr<IDXGIResource> AcquiredBuffer;
            AcquiredBuffer.Attach(pSurface);
            // ==============================
            // TODO: Process the frame here
            //
            // This is the most performance-critical section of code in an IddCx driver. It's important that whatever
            // is done with the acquired surface be finished as quickly as possible. This operation could be:
            //  * a GPU copy to another buffer surface for later processing (such as a staging surface for mapping to CPU memory)
            //  * a GPU encode operation
            //  * a GPU VPBlt to another surface
            //  * a GPU custom compute shader encode operation
            // ==============================

            // =========================================================
            AcquiredBuffer.Reset();
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }
        }
        else
        {
            if (hr == DXGI_ERROR_ACCESS_LOST && retryCount < maxRetries)
            {
                currentRetryDelay = min(currentRetryDelay + retryDelay, maxRetryDelay);
//                LOG_WARN("display[%d] DXGI_ERROR_ACCESS_LOST detected. Retry %d/%d after %dms delay.",
//                         m_InitParam.nConnectorIndex,retryCount + 1, maxRetries, currentRetryDelay);
                Sleep(currentRetryDelay);
                retryCount++;
                continue;
            }
            else
            {
                if (hr == DXGI_ERROR_ACCESS_LOST)
                {
                    LOG_ERROR("display[%d] DXGI_ERROR_ACCESS_LOST: Maximum retries (%d) reached. Exiting loop.", m_InitParam.nConnectorIndex, maxRetries);
                }
                else
                {
                    LOG_ERROR("display[%d] Failed to acquire buffer. Exiting loop. HRESULT:0x%08x", m_InitParam.nConnectorIndex, hr);
                }
                break;
            }
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :m_WdfDevice(WdfDevice)
{
    m_Adapter = {};
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
    // ==============================
    // TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
    // numbers are used for telemetry and may be displayed to the user in some situations.
    //
    // This is also where static per-adapter capabilities are determined.
    // ==============================

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);
    //AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER | IDDCX_ADAPTER_FLAGS_CAN_USE_MOVE_REGIONS;
//    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_USE_MOVE_REGIONS; //矩形移动区域支持 //这个会导致二次连接远程桌面时，窗口内黑屏
//    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;//使用最小模式
//    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER;//远程会话标志
//    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_PREFER_PHYSICAL_GPU;//偏好物理 GPU 渲染 iddcx 2.0版本以上支持
    //if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {//开启HDR支持 iddcx 2.0版本以上api支持
    //    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16 ;
    //    logStream << "FP16 processing capability detected.";
    //}
//    AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_USE_MOVE_REGIONS ;

    // Declare basic feature support for the adapter (required)
    AdapterCaps.MaxMonitorsSupported = monitorMaxCount;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // Declare your device strings for telemetry (required)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"Virtual Display Device";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"NztVdd";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"Virtual Display Model";

    // Declare your hardware and firmware versions (required)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    Version.MinorVer = 0;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    // Initialize a WDF context that can store a pointer to the device context object
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    // Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if (NT_SUCCESS(Status))
    {
        LOG_INFO("Init AdapterObject=0x%p", AdapterInitOut.AdapterObject);
        // Store a reference to the WDF adapter handle
        m_Adapter = AdapterInitOut.AdapterObject;

        // Store the device context object into the WDF object context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
    else
    {
        LOG_ERROR("IddCxAdapterInitAsync error: 0x%08x", Status);
        return;
    }
}
void IndirectDeviceContext::FinishInit(){
    Options.Adapter.apply(m_Adapter);
    for (unsigned int i = 0; i < displayCount; i++) {
        CreateMonitor(i);
    }
}


void IndirectDeviceContext::CreateMonitor(UINT displayIndex)
{
    LOG_DEBUG("开始创建虚拟显示器，index:%d", displayIndex);
    // ==============================
    // TODO: In a real driver, the EDID should be retrieved dynamically from a connected physical monitor. The EDIDs
    // provided here are purely for demonstration.
    // Monitor manufacturers are required to correctly fill in physical monitor attributes in order to allow the OS
    // to optimize settings like viewing distance and scale factor. Manufacturers should also use a unique serial
    // number every single device to ensure the OS can tell the monitors apart.
    // ==============================
    g_CurConnectorIndex = displayIndex;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectMonitorContextWrapper);
    attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto* pContext = WdfObjectGet_IndirectMonitorContextWrapper(Object);
        LOG_INFO("EvtCleanupCallback Monitor Object: 0x%p", Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    // In the sample driver, we report a monitor right away but a real driver would do this when a monitor connection event occurs
    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = displayIndex;
    monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    /* if the datasize is 0, then the os will call VirtualDisplayMonitorGetDefaultModes instead of VirtualDisplayParseMonitorDescription */
    if (!displays[displayIndex].use_edid)
    {
        monitorInfo.MonitorDescription.DataSize = 0;
        monitorInfo.MonitorDescription.pData = nullptr;
    }
    else
    {
        monitorInfo.MonitorDescription.DataSize = (UINT)displays[displayIndex].edid_len;
        monitorInfo.MonitorDescription.pData = displays[displayIndex].edid_data;
    }

    // ==============================
    // TODO: The monitor's container ID should be distinct from "this" device's container ID if the monitor is not
    // permanently attached to the display adapter device object. The container ID is typically made unique for each
    // monitor and can be used to associate the monitor with other devices, like audio or input devices. In this
    // sample we generate a random container ID GUID, but it's best practice to choose a stable container ID for a
    // unique monitor or to use "this" device's container ID for a permanent/integrated monitor.
    // ==============================

    // Create a container ID
    CoCreateGuid(&monitorInfo.MonitorContainerId);
    LOG_DEBUG("显示器[%d]的guid:%s",monitorInfo.ConnectorIndex, GuidToString(monitorInfo.MonitorContainerId).c_str());
    IDARG_IN_MONITORCREATE monitorCreate = {};
    monitorCreate.ObjectAttributes = &attr;
    monitorCreate.pMonitorInfo = &monitorInfo;
    // Create a monitor object with the specified monitor descriptor
    IDARG_OUT_MONITORCREATE monitorCreateOut;
   
    NTSTATUS status = IddCxMonitorCreate(m_Adapter, &monitorCreate, &monitorCreateOut);
    if (NT_SUCCESS(status))
    {
        auto monitor= monitorCreateOut.MonitorObject;
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(monitor);
        pMonitorContextWrapper->pContext = new IndirectMonitorContext(monitorInfo.ConnectorIndex, monitor, m_Adapter);

        // Tell the OS that the monitor has been plugged in
        IDARG_OUT_MONITORARRIVAL arrivalOut;
        LOG_DEBUG("display[%d] IddCxMonitorArrival MonitorObject=0x%p start", monitorInfo.ConnectorIndex, monitor);
        status = IddCxMonitorArrival(monitor, &arrivalOut);
        LOG_DEBUG("display[%d] IddCxMonitorArrival MonitorObject=0x%p end    luid:%s targetId:%d", monitorInfo.ConnectorIndex, monitorCreateOut.MonitorObject,
            (std::to_string(arrivalOut.OsAdapterLuid.LowPart) + "-" + std::to_string(arrivalOut.OsAdapterLuid.HighPart)).c_str(),
            arrivalOut.OsTargetId);
        if (!NT_SUCCESS(status))
        {
            switch (status)
            {
                case STATUS_INVALID_PARAMETER:
                    LOG_ERROR("display[%d] IddCxMonitorArrival error: 0x%08x(STATUS_INVALID_PARAMETER)", monitorInfo.ConnectorIndex, status);
                    break;
                case STATUS_NOT_SUPPORTED:
                    LOG_ERROR("display[%d] IddCxMonitorArrival error: 0x%08x(STATUS_NOT_SUPPORTED)", monitorInfo.ConnectorIndex, status);
                    break;
                default:
                    LOG_ERROR("display[%d] IddCxMonitorArrival error: 0x%08x", monitorInfo.ConnectorIndex, status);
                    break;
            }
        }
//        m_iddMonitor[displayIndex] = monitor;
    }
    else
    {
        LOG_ERROR("display[%d] IddCxMonitorCreate error: 0x%08x", displayIndex, status);
    }
    LOG_DEBUG("创建虚拟显示器结束，index:%d", displayIndex);
}

//void IndirectDeviceContext::DestroyMonitor(UINT displayIndex) {
//    if(m_iddMonitor[displayIndex]){
//        IddCxMonitorDeparture(m_iddMonitor[displayIndex]);
//        m_iddMonitor[displayIndex] = nullptr;
//    }
//}
//
//void IndirectDeviceContext::DestroyMonitors() {
//    for (int i = displayCount - 1; i >= 0; i--)
//        DestroyMonitor(i);
//}
//void IndirectDeviceContext::CommitModes(_In_ const IDARG_IN_COMMITMODES* pInArgs)
//{
//    for (UINT i = 0; i < pInArgs->PathCount; i++) {
//        auto path = &(pInArgs->pPaths[i]);
////        if (path->Flags == IDDCX_PATH_FLAGS_ACTIVE) {
//            auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(path->MonitorObject);
//            if (pMonitorContextWrapper && pMonitorContextWrapper->pContext) {
//                pMonitorContextWrapper->pContext->CommitModes(&path->TargetVideoSignalInfo);
//            }
//            else {
//                LOG_INFO("Get MonitorObject=0x%p NULL", path->MonitorObject);
//            }
////        }
//    }
//}

IndirectMonitorContext::IndirectMonitorContext(_In_ UINT ConnectorIndex, _In_ IDDCX_MONITOR Monitor, _In_ IDDCX_ADAPTER Adapter) :
m_ConnectorIndex(ConnectorIndex), m_Monitor(Monitor), m_Adapter(Adapter)
{
    m_hCursorEvent = nullptr;// CreateEvent(nullptr, false, false, nullptr);
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    UnassignSwapChain();
}

//void IndirectMonitorContext::CommitModes(_In_ const DISPLAYCONFIG_VIDEO_SIGNAL_INFO* pInfo)
//{
//    LOG_INFO("display[%d] totalSize=%dx%d totalSize=%dx%d hSyncFreq=%d/%d vSyncFreq=%d/%d"
//        " pixelRate=%d scanLineOrdering=%d vSyncFreqDivider=%d videoStandard=%d", m_ConnectorIndex,
//        pInfo->totalSize.cx, pInfo->totalSize.cy,
//        pInfo->activeSize.cx, pInfo->activeSize.cy,
//        pInfo->hSyncFreq.Numerator, pInfo->hSyncFreq.Denominator,
//        pInfo->vSyncFreq.Numerator, pInfo->vSyncFreq.Denominator,
//        pInfo->pixelRate, pInfo->scanLineOrdering,
//        pInfo->AdditionalSignalInfo.vSyncFreqDivider, pInfo->AdditionalSignalInfo.videoStandard
//    );
//    if (pInfo->vSyncFreq.Denominator > 0) {
//        m_nFrameRate = int((pInfo->vSyncFreq.Numerator * 1.0) / pInfo->vSyncFreq.Denominator);
//    }
//}

bool IndirectMonitorContext::SetHwCursorMode()
{
    if(m_hCursorEvent!=nullptr)
        return false;
    string name="VirtualDisplayMouse"+std::to_string(m_ConnectorIndex);
    auto mouseEvent = CreateEventA(nullptr,false,false,name.c_str());
    if (!mouseEvent){
        LOG_ERROR("Failed to create mouse event. No hardware cursor supported!");
        return false;
    }
//    LOG_DEBUG("开始设置硬件鼠标，index:%d", m_ConnectorIndex);
    IDARG_IN_SETUP_HWCURSOR hwCursor = {};
    hwCursor.hNewCursorDataAvailable = mouseEvent;
    hwCursor.CursorInfo.Size = sizeof(IDDCX_CURSOR_CAPS);
    hwCursor.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
    hwCursor.CursorInfo.AlphaCursorSupport = TRUE;
    //在大多数情况下，128是可以的，但为了安全起见，我们将使用512，较旧的英特尔CPU可能限制为64x64
    hwCursor.CursorInfo.MaxX = 128;
    hwCursor.CursorInfo.MaxY = 128;

    NTSTATUS Status = IddCxMonitorSetupHardwareCursor(m_Monitor, &hwCursor);
    if (!NT_SUCCESS(Status)) {
        LOG_ERROR("display[%d]IddCxMonitorSetupHardwareCursor error: 0x%08x", m_ConnectorIndex, Status);
        return false;
    }
    m_hCursorEvent=mouseEvent;
    LOG_DEBUG("设置硬件鼠标成功，index:%d", m_ConnectorIndex);
    return true;
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    UnassignSwapChain();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
        // swap-chain and try again.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        // Create a new swap-chain processing thread
        InitChainParam param;
        param.hAdapter = m_Adapter;
        param.hMonitor = m_Monitor;
        param.nConnectorIndex = m_ConnectorIndex;
//        param.nFramerate = m_nFrameRate;
        m_ProcessingThread.reset(new SwapChainProcessor(this,param, SwapChain, Device, NewFrameEvent));
    }
//    LOG_DEBUG("完成创建交换链=>%d",m_ConnectorIndex);
}

void IndirectMonitorContext::UnassignSwapChain()
{
//    pauseStream(m_ConnectorIndex);
    if(m_ProcessingThread){
        m_ProcessingThread.reset();
    }
    if (m_hCursorEvent) {
        CloseHandle(m_hCursorEvent);
        m_hCursorEvent = nullptr;
    }
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS VirtualDisplayAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    //UNREFERENCED_PARAMETER(AdapterObject);
    //UNREFERENCED_PARAMETER(pInArgs);
    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (NULL == pDeviceContextWrapper || NULL == pDeviceContextWrapper->pContext) {
        LOG_ERROR("Get AdapterObject=0x%p NULL", AdapterObject);
        return STATUS_INVALID_PARAMETER;
    }

    // This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
    // to report attached monitors.

    LOG_DEBUG("call pContext->FinishInit start");
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        pDeviceContextWrapper->pContext->FinishInit();
        PipeWorkingContext.adapter=AdapterObject;//设置当前显示适配器
        LOG_DEBUG( "Adapter initialization finished successfully.");
    }else {
        LOG_ERROR("AdapterInitStatus=0x%08x", pInArgs->AdapterInitStatus);
    }
    LOG_DEBUG("call pContext->FinishInit end");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    //auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    //if (NULL == pDeviceContextWrapper || NULL == pDeviceContextWrapper->pContext) {
    //    LOG_ERROR("Get AdapterObject=0x%p NULL", AdapterObject);
    //    return STATUS_INVALID_PARAMETER;
    //}
    ////pInArgs->PathCount
    //LOG_DEBUG("打印参数：路径数量：%d", pInArgs->PathCount);

    //  对于示例，当模式被选择时，不执行任何操作 - 交换链由IddCx处理
    // ==============================
    // TODO: 在实际的驱动程序中，此函数将用于重新配置设备以应用新模式。 Loop
    // 遍历pInArgs->pPaths并查找IDDCX_PATH_FLAGS_ACTIVE。
    // 任何未处于活动状态的路径均为非活动状态（例如，显示器应处于关闭状态）。
    // ==============================
    //    pDeviceContextWrapper->pContext->CommitModes(pInArgs);

        //for (UINT i = 0; i < pInArgs->PathCount; i++) {
        //            auto path = &(pInArgs->pPaths[i]);
        //    //        if (path->Flags == IDDCX_PATH_FLAGS_ACTIVE) {
        //                auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(path->MonitorObject);
        //                if (pMonitorContextWrapper && pMonitorContextWrapper->pContext) {
        //                    auto pInfo = &path->TargetVideoSignalInfo;
        //                    LOG_INFO("display[%d] totalSize=%dx%d activeSize=%dx%d hSyncFreq=%d/%d vSyncFreq=%d/%d"
        //                         " pixelRate=%d scanLineOrdering=%d vSyncFreqDivider=%d videoStandard=%d", i,
        //                         pInfo->totalSize.cx, pInfo->totalSize.cy,
        //                         pInfo->activeSize.cx, pInfo->activeSize.cy,
        //                         pInfo->hSyncFreq.Numerator, pInfo->hSyncFreq.Denominator,
        //                         pInfo->vSyncFreq.Numerator, pInfo->vSyncFreq.Denominator,
        //                         pInfo->pixelRate, pInfo->scanLineOrdering,
        //                         pInfo->AdditionalSignalInfo.vSyncFreqDivider, pInfo->AdditionalSignalInfo.videoStandard
        //                    );
        //                }
        //                else {
        //                    LOG_INFO("Get MonitorObject=0x%p NULL", path->MonitorObject);
        //                }
        //    //        }
        //        }





    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
    // this sample driver, we hard-code the EDID, so this function can generate known modes.
    // ==============================
    // edid: pInArgs->MonitorDescription.pData, pInArgs->MonitorDescription.DataSize
    LOG_DEBUG("display[%d] pInArgs->MonitorModeBufferInputCount=%d, call VirtualDisplayParseMonitorDescription", g_CurConnectorIndex, pInArgs->MonitorModeBufferInputCount);
    DisplayInfo* display_info = &displays[g_CurConnectorIndex];
    pOutArgs->MonitorModeBufferOutputCount = UINT(display_info->size_cnt * display_info->fps_cnt);
    if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
    {
        // Return success if there was no buffer, since the caller was only asking for a count of modes
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    else
    {
        // In the sample driver, we have reported some static information about connected monitors
        // Check which of the reported monitors this call is for by comparing it to the pointer of
        // our known EDID blocks.

        int nIndex = 0;
        int bestIndex = 0;
        for (int item_idx = 0; item_idx < display_info->size_cnt; item_idx++) {
            auto item = display_info->size_list[item_idx];
            for (int fps_idx = 0; fps_idx < display_info->fps_cnt; fps_idx++) {
                auto fps = display_info->fps_list[fps_idx];
                LOG_DEBUG("正在处理分辨率 %dx%d@%d index:%d", item.cx, item.cy, fps, nIndex);
                if (item.cx == display_info->prefer_width && item.cy == display_info->prefer_height &&
                    fps == display_info->prefer_fps) {
                    pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode(item.cx, item.cy, fps);
                    LOG_DEBUG("设置当前分辨率为推荐分辨率 %dx%d@%d", item.cx, item.cy, fps);
                    nIndex++;
                    break;
                }
            }
        }
        if(nIndex==0){
            display_info->prefer_width=1920;
            display_info->prefer_height=1080;
            display_info->prefer_fps=60;
            pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode(display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
            LOG_DEBUG("分辨率配置错误，设置默认分辨率 %dx%d@%d", display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
        }
//                pInArgs->pMonitorModes[nIndex] = CreateIddCxMonitorMode(item.cx, item.cy, fps);
//                if (item.cx == display_info->prefer_width && item.cy == display_info->prefer_height && fps == display_info->prefer_fps) {
//                    bestIndex = nIndex;
//                    LOG_DEBUG("设置当前分辨率为推荐分辨率 %dx%d@%d",item.cx,item.cy,fps);
//                }
//                nIndex++;
//            }
//        }
//        if (bestIndex == -1) {
//            LOG_DEBUG("未匹配到合适的分辨率，默认使用第一个");
//            bestIndex = 0;
//        }
        pOutArgs->PreferredMonitorModeIdx = bestIndex;
        return STATUS_SUCCESS;
    }
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    LOG_DEBUG("未使用edid加载，使用简单显示配置支持===================>");
    UNREFERENCED_PARAMETER(MonitorObject);
//    UNREFERENCED_PARAMETER(pInArgs);
//    UNREFERENCED_PARAMETER(pOutArgs);
    // ==============================
    // TODO: 驱动程序应报告传输协议和几乎所有监视器（如640x480、800x600或1024x768）都保证支持的模式。如果驾驶员可以从EDID以外的描述符访问监控模式，则这些模式也将在此处报告。
    // ==============================
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        pOutArgs->DefaultMonitorModeBufferOutputCount = 1; //默认我们只支持一个默认
    }
    else
    {
        auto config = displayResFpsList[0];
        pInArgs->pDefaultMonitorModes[0] = CreateIddCxMonitorMode(get<0>(config), get<1>(config), get<2>(config));
        pOutArgs->DefaultMonitorModeBufferOutputCount = 1;
        pOutArgs->PreferredMonitorModeIdx = 0;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    //UNREFERENCED_PARAMETER(MonitorObject);
    //UNREFERENCED_PARAMETER(pInArgs);
    //UNREFERENCED_PARAMETER(pOutArgs);
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (NULL == pMonitorContextWrapper || NULL == pMonitorContextWrapper->pContext) {
        LOG_ERROR("Get MonitorObject=0x%p NULL", MonitorObject);
        return STATUS_INVALID_PARAMETER;
    }

    auto display_idx = pMonitorContextWrapper->pContext->GetConnectorIndex();
    DisplayInfo *display_info = &displays[display_idx];

    //vector<IDDCX_TARGET_MODE> TargetModes;

    // Create a set of modes supported for frame processing and scan-out. These are typically not based on the
    // monitor's descriptor and instead are based on the static processing capability of the device. The OS will
    // report the available set of modes for a given output as the intersection of monitor modes with target modes.
    // edid: pInArgs->MonitorDescription.pData pInArgs->MonitorDescription.DataSize

    // 获取缓冲区大小
    pOutArgs->TargetModeBufferOutputCount = UINT(display_info->size_cnt * display_info->fps_cnt);
    LOG_DEBUG("display[%d] TargetModeBufferInputCount=%d,TargetModeBufferOutputCount=%d, call VirtualDisplayMonitorQueryModes",
        display_idx, pInArgs->TargetModeBufferInputCount, pOutArgs->TargetModeBufferOutputCount);
    if (pInArgs->TargetModeBufferInputCount == 0) {/* 用于设置缓冲区大小 */
        return STATUS_SUCCESS;
    }
    else if (pInArgs->TargetModeBufferInputCount < pOutArgs->TargetModeBufferOutputCount) {
        LOG_INFO("display[%d] STATUS_BUFFER_TOO_SMALL", display_idx);
        return STATUS_BUFFER_TOO_SMALL;
    }
    else {
        int nIndex = 0;
        pInArgs->pTargetModes[nIndex] = CreateIddCxTargetMode(display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps);
        LOG_DEBUG("创建虚拟显示器目标模式:%dx%d@%d TargetMode index %d", display_info->prefer_width, display_info->prefer_height, display_info->prefer_fps, nIndex)
//        for (int item_idx = 0; item_idx < display_info->size_cnt; item_idx++) {
//            auto item = display_info->size_list[item_idx];
//            for (int fps_idx = 0; fps_idx < display_info->fps_cnt; fps_idx++) {
//                auto fps = display_info->fps_list[fps_idx];
//                pInArgs->pTargetModes[nIndex] = CreateIddCxTargetMode(item.cx, item.cy, fps);
//                LOG_DEBUG("创建虚拟显示器目标模式:%dx%d@%d TargetMode index %d", item.cx, item.cy, fps, nIndex)
//                nIndex++;
//            }
//        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    //UNREFERENCED_PARAMETER(MonitorObject);
    //UNREFERENCED_PARAMETER(pInArgs);
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (NULL == pMonitorContextWrapper || NULL == pMonitorContextWrapper->pContext) {
        LOG_ERROR("Get MonitorObject=0x%p NULL", MonitorObject);
        return STATUS_INVALID_PARAMETER;
    }
    auto display_idx = pMonitorContextWrapper->pContext->GetConnectorIndex();
    LOG_DEBUG("display[%d] call pContext->AssignSwapChain", display_idx);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    //UNREFERENCED_PARAMETER(MonitorObject);
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (NULL == pMonitorContextWrapper || NULL == pMonitorContextWrapper->pContext) {
        LOG_ERROR("Get MonitorObject=0x%p NULL", MonitorObject);
        return STATUS_INVALID_PARAMETER;
    }
    auto display_idx = pMonitorContextWrapper->pContext->GetConnectorIndex();
    LOG_DEBUG("display[%d] call pContext->UnassignSwapChain", display_idx);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayMonitorSetGammaRamp(IDDCX_MONITOR MonitorObject, const IDARG_IN_SET_GAMMARAMP* pInArgs)
{
    LOG_INFO("VirtualDisplayMonitorSetGammaRamp===============>");
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    return STATUS_SUCCESS;
}

#pragma endregion


#pragma region Pipe相关

#define PIPE_NAME L"\\\\.\\pipe\\NztVdd"

HANDLE hPipeThread = NULL;
bool g_Running = true;
mutex g_Mutex;
HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;

void  SendToPipe(const std::string& logMessage) {
    if (g_pipeHandle != INVALID_HANDLE_VALUE) {
        DWORD bytesWritten;
        DWORD logMessageSize = static_cast<DWORD>(logMessage.size());
        WriteFile(g_pipeHandle, logMessage.c_str(), logMessageSize, &bytesWritten, NULL);
    }
}

void ReloadDriver() {
        // 驱动内部收到 RELOAD 命令时检查
        if (GetSystemMetrics(SM_REMOTESESSION)) {          // 当前处于 RDP 远程桌面环境！
            SendToPipe("windows using RDP now,Reload reject!!!");
            LOG_WARN("windows using RDP now,Reload reject!!!");
            return;
        }
        LOG_INFO("Adapter will reinitialize...");
        if(PipeWorkingContext.adapter!= nullptr) {
            auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(PipeWorkingContext.adapter);
            if (pContext != nullptr) {
                WDFDEVICE wdfDevice = (WDFDEVICE)WdfObjectContextGetObject(pContext);// 使用框架宏，直接反向获取绑定的 WDFDEVICE
                if (wdfDevice) {
                    SendToPipe("ReloadDriver working...");
//                    pContext->pContext->DestroyMonitors();//拔除显示器 todo:拔除会造成新的问题。
                    PipeWorkingContext.adapter = nullptr;//清理全局指针
                    WdfDeviceSetFailed(wdfDevice, WdfDeviceFailedAttemptRestart);   // 现在你可以安全地调用了
                    LOG_INFO("Adapter reinitialized");//这句理论上永远无法调用执行，因为已经重新加载了
                    return;
                }
            }
        }
        LOG_WARN("Adapter reinitialize failed,context is null");
}

void LogIddCxVersion() {
    IDARG_OUT_GETVERSION outArgs;
    NTSTATUS status = IddCxGetVersion(&outArgs);

    if (NT_SUCCESS(status)) {
        char versionStr[16];
        sprintf_s(versionStr, "0x%lx", outArgs.IddCxVersion);
        string logMessage = "IDDCX Version: " + string(versionStr);
        LOG_INFO(logMessage.c_str());
    }
    else {
        LOG_ERROR("Failed to get IDDCX version");
    }
}

LUID getSetAdapterLuid() {
    AdapterOption& adapterOption = Options.Adapter;
    if (!adapterOption.hasTargetAdapter) {
        LOG_ERROR("No Gpu Found/Selected");
    }
    return adapterOption.adapterLuid;
}


void GetGpuInfo()
{
    AdapterOption& adapterOption = Options.Adapter;
    if (!adapterOption.hasTargetAdapter) {
        LOG_ERROR("No GPU found or set.");
        return;
    }
    try {
        string utf8_desc = WStringToString(adapterOption.target_name);
        LUID luid = getSetAdapterLuid();
        string logtext = "ASSIGNED GPU: " + utf8_desc +" (LUID: " + std::to_string(luid.LowPart) + "-" + std::to_string(luid.HighPart) + ")";
        LOG_INFO( logtext.c_str());
    }
    catch (const exception& e) {
        LOG_ERROR(("Error: " + string(e.what())).c_str());
    }
}

void logAvailableGPUs() {
    vector<GPUInfo> gpus;
    ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return;
    }
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter> adapter;
        if (!SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
            break;
        }
        DXGI_ADAPTER_DESC desc;
        if (!SUCCEEDED(adapter->GetDesc(&desc))) {
            continue;
        }
        GPUInfo gpuInfo{ desc.Description, adapter, desc };
        gpus.push_back(gpuInfo);
    }
    for (const auto& gpu : gpus) {
        wstring logMessage = L"GPU Name: ";
        logMessage += gpu.desc.Description;
        wstring memorySize = L" Memory: ";
        memorySize += std::to_wstring(gpu.desc.DedicatedVideoMemory / (1024 * 1024)) + L" MB";
        wstring logText = logMessage + memorySize;
        int bufferSize = WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (bufferSize > 0) {
            std::string logTextA(bufferSize - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, logText.c_str(), -1, &logTextA[0], bufferSize, nullptr, nullptr);
            LOG_INFO(logTextA.c_str());
        }
    }
}

void HandleClient() {
    g_pipeHandle = PipeWorkingContext.hPipe; //pWorkContext->hPipe;
    LOG_DEBUG("Client Handling Enabled");
    wchar_t buffer[128];
    DWORD bytesRead;
    BOOL result = ReadFile(g_pipeHandle, buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, NULL);
    if (result && bytesRead != 0) {
        buffer[bytesRead / sizeof(wchar_t)] = L'\0';
        wstring bufferwstr(buffer);
        int bufferSize = WideCharToMultiByte(CP_UTF8, 0, bufferwstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        string bufferstr(bufferSize, 0);
        WideCharToMultiByte(CP_UTF8, 0, bufferwstr.c_str(), -1, &bufferstr[0], bufferSize, nullptr, nullptr);
        LOG_INFO(bufferstr.c_str());
        if (wcsncmp(buffer, L"RELOAD_DRIVER", 13) == 0) {
            LOG_DEBUG("Reloading the driver");
            ReloadDriver();
        }
        else if (wcsncmp(buffer, L"LOG_DEBUG", 9) == 0) {
            wchar_t* param = buffer + 10;
            if (wcsncmp(param, L"true", 4) == 0) {
//                UpdateXmlToggleSetting(true, L"debug logging");
                logLevel = 4;
                LOG_INFO("Pipe debugging enabled");
            }
            else if (wcsncmp(param, L"false", 5) == 0) {
//                UpdateXmlToggleSetting(false, L"debuglogging");
                logLevel = 3;
                LOG_INFO("Debugging disabled");
            }
        //}
        //else if (wcsncmp(buffer, L"LOGGING", 7) == 0) {
        //    wchar_t* param = buffer + 8;
        //    if (wcsncmp(param, L"true", 4) == 0) {
        //        UpdateXmlToggleSetting(true, L"logging");
        //        logsEnabled = true;
        //        LOG_INFO("Logging Enabled");
        //    }
        //    else if (wcsncmp(param, L"false", 5) == 0) {
        //        UpdateXmlToggleSetting(false, L"logging");
        //        logsEnabled = false;
        //        LOG_INFO("Logging disabled"); // We can keep this here just to make it delete the logs on disable
        //    }
        //}
        //else if (wcsncmp(buffer, L"HDRPLUS", 7) == 0) {
        //    wchar_t* param = buffer + 8;
        //    if (wcsncmp(param, L"true", 4) == 0) {
        //        UpdateXmlToggleSetting(true, L"HDRPlus");
        //        LOG_INFO("HDR+ Enabled");
        //        ReloadDriver(hPipe);
        //    }
        //    else if (wcsncmp(param, L"false", 5) == 0) {
        //        UpdateXmlToggleSetting(false, L"HDRPlus");
        //        LOG_INFO("HDR+ Disabled");
        //        ReloadDriver(hPipe);
        //    }
        //}
        //else if (wcsncmp(buffer, L"SDR10", 5) == 0) {
        //    wchar_t* param = buffer + 6;
        //    if (wcsncmp(param, L"true", 4) == 0) {
        //        UpdateXmlToggleSetting(true, L"SDR10bit");
        //        LOG_INFO("SDR 10 Bit Enabled");
        //        ReloadDriver(hPipe);
        //    }
        //    else if (wcsncmp(param, L"false", 5) == 0) {
        //        UpdateXmlToggleSetting(false, L"SDR10bit");
        //        LOG_INFO("SDR 10 Bit Disabled");
        //        ReloadDriver(hPipe);
        //    }
        //}
        //else if (wcsncmp(buffer, L"CUSTOMEDID", 10) == 0) {
        //    wchar_t* param = buffer + 11;
        //    if (wcsncmp(param, L"true", 4) == 0) {
        //        UpdateXmlToggleSetting(true, L"CustomEdid");
        //        LOG_INFO("Custom Edid Enabled");
        //        ReloadDriver(hPipe);
        //    }
        //    else if (wcsncmp(param, L"false", 5) == 0) {
        //        UpdateXmlToggleSetting(false, L"CustomEdid");
        //        LOG_INFO("Custom Edid Disabled");
        //        ReloadDriver(hPipe);
        //    }
        //}
        //else if (wcsncmp(buffer, L"PREVENTSPOOF", 12) == 0) {
        //    wchar_t* param = buffer + 13;
        //    if (wcsncmp(param, L"true", 4) == 0) {
        //        UpdateXmlToggleSetting(true, L"PreventSpoof");
        //        LOG_INFO("Prevent Spoof Enabled");
        //        ReloadDriver(hPipe);
        //    }
        //    else if (wcsncmp(param, L"false", 5) == 0) {
        //        UpdateXmlToggleSetting(false, L"PreventSpoof");
        //        LOG_INFO("Prevent Spoof Disabled");
        //        ReloadDriver(hPipe);
        //    }
        //}
        //else if (wcsncmp(buffer, L"CEAOVERRIDE", 11) == 0) {
        //    wchar_t* param = buffer + 12;
        //    if (wcsncmp(param, L"true", 4) == 0) {
        //        UpdateXmlToggleSetting(true, L"EdidCeaOverride");
        //        LOG_INFO("Cea override Enabled");
        //        ReloadDriver(hPipe);
        //    }
        //    else if (wcsncmp(param, L"false", 5) == 0) {
        //        UpdateXmlToggleSetting(false, L"EdidCeaOverride");
        //        LOG_INFO("Cea override Disabled");
        //        ReloadDriver(hPipe);
        //    }
        //}
        //else if (wcsncmp(buffer, L"HARDWARECURSOR", 14) == 0) {
        //    wchar_t* param = buffer + 15;
        //    if (wcsncmp(param, L"true", 4) == 0) {
        //        UpdateXmlToggleSetting(true, L"HardwareCursor");
        //        LOG_INFO("Hardware Cursor Enabled");
        //        ReloadDriver(hPipe);
        //    }
        //    else if (wcsncmp(param, L"false", 5) == 0) {
        //        UpdateXmlToggleSetting(false, L"HardwareCursor");
        //        LOG_INFO("Hardware Cursor Disabled");
        //        ReloadDriver(hPipe);
        //    }
        //}
        //else if (wcsncmp(buffer, L"D3DDEVICEGPU", 12) == 0) {
        //    LOG_INFO("Retrieving D3D GPU (This information may be inaccurate without reloading the driver first)");
        //    InitializeD3DDeviceAndLogGPU();
        //    LOG_INFO("Retrieved D3D GPU");
        }else if (wcsncmp(buffer, L"IDDCX_VERSION", 12) == 0) {
            LOG_INFO("Logging iddcx version");
            LogIddCxVersion();
        }
        else if (wcsncmp(buffer, L"GET_GPUINFO", 14) == 0) {
            LOG_INFO("Retrieving Assigned GPU");
            GetGpuInfo();
            LOG_INFO("Retrieved Assigned GPU");
        }else if (wcsncmp(buffer, L"GET_GPUS", 10) == 0) {
            LOG_INFO("Logging all GPUs");
            LOG_INFO( "Any GPUs which show twice but you only have one, will most likely be the GPU the driver is attached to");
            logAvailableGPUs();
            LOG_INFO("Logged all GPUs");
//        }else if (wcsncmp(buffer, L"SETGPU", 6) == 0) { //设置指定的gpu，暂时不提供支持
//            std::wstring gpuName = buffer + 7;
//            gpuName = gpuName.substr(1, gpuName.size() - 2);
//
//            int size_needed = WideCharToMultiByte(CP_UTF8, 0, gpuName.c_str(), static_cast<int>(gpuName.length()), nullptr, 0, nullptr, nullptr);
//            std::string gpuNameNarrow(size_needed, 0);
//            WideCharToMultiByte(CP_UTF8, 0, gpuName.c_str(), static_cast<int>(gpuName.length()), &gpuNameNarrow[0], size_needed, nullptr, nullptr);
//
//            LOG_INFO(("Setting GPU to: " + gpuNameNarrow).c_str());
//            if (UpdateXmlGpuSetting(gpuName.c_str())) {
//                LOG_INFO("Gpu Changed, Restarting Driver");
//            }
//            else {
//                LOG_ERROR( "Failed to update GPU setting in XML. Restarting anyway");
//            }
//            ReloadDriver(hPipe);
//        }else if (wcsncmp(buffer, L"SETDISPLAYCOUNT", 15) == 0) { //暂不提供通过修改配置来重新加载驱动的支持
//            LOG_INFO( "Setting Display Count");
//
//            int newDisplayCount = 1;
//            swscanf_s(buffer + 15, L"%d", &newDisplayCount);
//
//            std::wstring displayLog = L"Setting display count  to " + std::to_wstring(newDisplayCount);
//            LOG_INFO(WStringToString(displayLog).c_str());
//
//            if (UpdateXmlDisplayCountSetting(newDisplayCount)){
//                LOG_INFO("Display Count Changed, Restarting Driver");
//            }
//            else {
//                LOG_ERROR( "Failed to update display count setting in XML. Restarting anyway");
//            }
//            ReloadDriver(hPipe);
        //}
        //else if (wcsncmp(buffer, L"GETSETTINGS", 11) == 0) {
        //    //query and return settings
        //    bool debugEnabled = EnabledQuery(L"DebugLoggingEnabled");
        //    bool loggingEnabled = EnabledQuery(L"LoggingEnabled");

        //    wstring settingsResponse = L"SETTINGS ";
        //    settingsResponse += debugEnabled ? L"DEBUG=true " : L"DEBUG=false ";
        //    settingsResponse += loggingEnabled ? L"LOG=true" : L"LOG=false";

        //    DWORD bytesWritten;
        //    DWORD bytesToWrite = static_cast<DWORD>((settingsResponse.length() + 1) * sizeof(wchar_t));
        //    WriteFile(hPipe, settingsResponse.c_str(), bytesToWrite, &bytesWritten, NULL);

        }
        else if (wcsncmp(buffer, L"PING", 4) == 0) {
            SendToPipe("PONG");
            LOG_INFO("Heartbeat Ping");
        }
        else {
            LOG_ERROR( "Unknown command");

            size_t size_needed;
            wcstombs_s(&size_needed, nullptr, 0, buffer, 0);
            std::string narrowString(size_needed, 0);
            wcstombs_s(nullptr, &narrowString[0], size_needed, buffer, size_needed);
            LOG_ERROR( narrowString.c_str());
        }
    }
    DisconnectNamedPipe(g_pipeHandle);
    CloseHandle(g_pipeHandle);
    g_pipeHandle = INVALID_HANDLE_VALUE; // This value determines whether or not all data gets sent back through the pipe or just the handling pipe data
}

DWORD WINAPI NamedPipeServer(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;
    const wchar_t *sddl = L"D:(A;;GA;;;WD)";
    LOG_INFO("Starting pipe with parameters: D:(A;;GA;;;WD)");
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
        DWORD ErrorCode = GetLastError();
        string errorMessage = to_string(ErrorCode);
        LOG_ERROR(errorMessage.c_str());
        return 1;
    }
    HANDLE hPipe;
    while (g_Running) {
        hPipe = CreateNamedPipeW(PIPE_NAME,PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,512, 512,0,&sa);

        if (hPipe == INVALID_HANDLE_VALUE) {
            DWORD ErrorCode = GetLastError();
            string errorMessage = to_string(ErrorCode);
            LOG_ERROR(errorMessage.c_str());
            LocalFree(sa.lpSecurityDescriptor);
            return 1;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            PipeWorkingContext.hPipe=hPipe;
            LOG_INFO("创建命名的Pipe[%s]成功！",WStringToString(PIPE_NAME).c_str());
            HandleClient();
        }
        else {
            LOG_ERROR("创建命名的Pipe[%s]失败！",WStringToString(PIPE_NAME).c_str());
            CloseHandle(hPipe);
        }
    }
    LocalFree(sa.lpSecurityDescriptor);
    return 0;
}

void StartNamedPipeServer() {
    LOG_INFO("Starting Pipe");
    hPipeThread = CreateThread(NULL, 0, NamedPipeServer, NULL, 0, NULL);
    if (hPipeThread == NULL) {
        DWORD ErrorCode = GetLastError();
        string errorMessage = to_string(ErrorCode);
        LOG_ERROR( errorMessage.c_str());
    }
    else {
        LOG_INFO("Pipe created");
    }
}

void StopNamedPipeServer() {
    LOG_INFO("Stopping Pipe");
    {
        lock_guard<mutex> lock(g_Mutex);
        g_Running = false;
    }
    if (hPipeThread) {
        HANDLE hPipe = CreateFileW(
                PIPE_NAME,
                GENERIC_READ | GENERIC_WRITE,
                0,
                NULL,
                OPEN_EXISTING,
                0,
                NULL);

        if (hPipe != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }

        WaitForSingleObject(hPipeThread, INFINITE);
        CloseHandle(hPipeThread);
        hPipeThread = NULL;
        LOG_INFO("Stopped Pipe");
    }
}


extern "C" EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;
//驱动卸载事件
VOID EvtDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    StopNamedPipeServer();
    LOG_INFO( "Driver Unloaded");
}

//驱动入口
_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT  pDriverObject,PUNICODE_STRING pRegistryPath)
{
    SetUnhandledExceptionFilter(exception_handler);
    init(); //加载配置
    string version=VERSION;
    LOG_INFO("call WdfDriverCreate!version:%s",version.c_str());

    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config,VirtualDisplayDeviceAdd);

    Config.EvtDriverUnload = EvtDriverUnload; //注册驱动卸载事件

    //todo：在这里做了大量的配置初始化，我们使用上面的init来初始化即可
    LogIddCxVersion();
    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        LOG_ERROR("WdfDriverCreate error: 0x%08x", Status);
        return Status;
    }
    StartNamedPipeServer();
    return Status;
}
#pragma endregion

_Use_decl_annotations_
NTSTATUS VirtualDisplayDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit){
    LOG_INFO("call IddCxDeviceInitConfig WdfDeviceCreate");

    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    // Register for power callbacks - in this sample only power-on is needed
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = VirtualDisplayDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);
    // If the driver wishes to handle custom IoDeviceControl requests,
    // it's necessary to use this callback since IddCx
    // redirects IoDeviceControl requests to an internal queue. This sample does not need this.
    // IddConfig.EvtIddCxDeviceIoControl = VirtualDisplayIoDeviceControl;

    GetGpuInfo();

    IddConfig.EvtIddCxAdapterInitFinished = VirtualDisplayAdapterInitFinished;

    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = VirtualDisplayMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = VirtualDisplayMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = VirtualDisplayMonitorUnassignSwapChain;
//    if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo){
//        LOG_ERROR("激活客户端配置失败！");
//
//EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 dd;
//                IddConfig.EvtIddCxAdapterQueryTargetInfo = VirtualDisplayDriverEvtIddCxAdapterQueryTargetInfo;
//        //        IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = VirtualDisplayDriverEvtIddCxMonitorSetDefaultHdrMetadata;
//        //        IddConfig.EvtIddCxParseMonitorDescription2 = VirtualDisplayDriverEvtIddCxParseMonitorDescription2;
//        //        IddConfig.EvtIddCxMonitorQueryTargetModes2 = VirtualDisplayDriverEvtIddCxMonitorQueryTargetModes2;
//        //        IddConfig.EvtIddCxAdapterCommitModes2 = VirtualDisplayDriverEvtIddCxAdapterCommitModes2;
//        IddConfig.EvtIddCxMonitorSetGammaRamp = VirtualDisplayMonitorSetGammaRamp;
//    }else{
        //windows 10的书写方式
        IddConfig.EvtIddCxParseMonitorDescription = VirtualDisplayParseMonitorDescription;
        IddConfig.EvtIddCxMonitorQueryTargetModes = VirtualDisplayMonitorQueryModes;
        IddConfig.EvtIddCxAdapterCommitModes = VirtualDisplayAdapterCommitModes;
        IddConfig.EvtIddCxMonitorSetGammaRamp = VirtualDisplayMonitorSetGammaRamp;
//    }

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status)){
        LOG_ERROR("IddCxDeviceInitConfig error: 0x%08x", Status);
        return Status;
    }
    LOG_DEBUG("IddCxDevice inited!");
    loadSettings();//我们在这里加载细节配置
//    initShareStream();

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object) {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        LOG_DEBUG("EvtCleanupCallback Device Object: 0x%p", Object);
        if (pContext) {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)){
        LOG_ERROR("WdfDeviceCreate error: 0x%08x", Status);
    return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    if (!NT_SUCCESS(Status)){
        LOG_ERROR("IddCxDeviceInitialize error: 0x%08x", Status);
    }

    // Create a new device context object and attach it to the WDF device object
    auto *pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    LOG_INFO("Create DeviceObject=0x%p suc", Device);
    return Status;
}