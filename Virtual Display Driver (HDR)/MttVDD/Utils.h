#pragma once

#include <windows.h>
#include <vector>
#include <stdio.h>
#include <string>
#include <iomanip>
#include <cctype>
#include <chrono>
#include <iostream>
#include <sstream>
#include <filesystem>

#include "base.h"

#ifndef REG_SZ_NODE
#define REG_SZ_NODE L"SOFTWARE\\CgTeamwork\\VirtualDisplayDriver"
#endif // !REG_SZ_NODE

enum class LogLevel{
    None,
    Error,
    Warning,
    Info,
    Debug
};
#define LOG_DEBUG(fmt, ...)     write_log("DEBUG", __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__);
#define LOG_INFO(fmt, ...)      write_log("INFO", __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__);
#define LOG_WARN(fmt, ...)      write_log("WARN", __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__);
#define LOG_ERROR(fmt, ...)     write_log("ERROR", __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__);

typedef void* frames_ptr;

extern "C" {
void write_log(const char* level, const char* func, int line, const char* format, ...);
void streamAddFrame(frames_ptr frames, const void* frame_buf, int frame_len);
}

std::string GuidToString(const GUID& guid);
std::string getCurrentDate();
int64_t getCurrentTime();
std::string WStringToString(const std::wstring& wstr);
std::wstring StringToWString(const std::string& str);

std::vector<std::string> split(std::string& input, char delimiter);

std::string& ltrimString(std::string& s,std::string words= "\f\v\r\t\n ");
std::string& rtrimString(std::string& s,std::string words = "\f\v\r\t\n ");
std::string& trimString(std::string& s, std::string words = "\f\v\r\t\n ");
std::string replaceString(const std::string& sourceStr,std::string matchStr, std::string replaceStr);

int indexOfString(std::string s, std::string words);
std::string& toLowerString(std::string& s);

#pragma region reg

std::wstring readFromRegString(std::wstring name, std::wstring subkey = REG_SZ_NODE, HKEY key = HKEY_LOCAL_MACHINE);
int readFromRegWord(std::wstring name, std::wstring subkey = REG_SZ_NODE, HKEY key = HKEY_LOCAL_MACHINE);

#pragma endregion