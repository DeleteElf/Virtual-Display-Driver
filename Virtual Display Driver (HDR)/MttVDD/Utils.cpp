#include "Utils.h"

//#include <fstream>
//#include <algorithm>
#include <combaseapi.h>
#include <codecvt>
using namespace std;
string getCurrentDate() {
    // 获取当前日期并以"YYYY-MM-DD"格式返回
    auto now = chrono::system_clock::now();
    auto in_time_t = chrono::system_clock::to_time_t(now);
    tm* out_time=new tm();
    localtime_s(out_time, &in_time_t);
    stringstream ss;
    ss << put_time(out_time, "%Y-%m-%d");
    return ss.str();
}

int64_t getCurrentTime(){
    auto now = chrono::system_clock::now().time_since_epoch();//chrono::high_resolution_clock::now();
    return chrono::duration_cast<chrono::milliseconds>(now).count();
}

string WStringToString(const wstring& wstr) { //basically just a function for converting strings since codecvt is depricated in c++ 17
    if (wstr.empty()) return "";

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
    //wstring_convert<codecvt_utf8<wchar_t>> conv;
    //string str = conv.to_bytes(wstr);
    return str;
}

wstring StringToWString(const string& str)
{
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    wstring wstr(size_needed,0);
	MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], size_needed);
//	return wstring(wstr.data(), wstr.data() + size_needed);
    return wstr;
}

vector<string> split(string& input, char delimiter)
{
    istringstream stream(input);
    string field;
    vector<string> result;
    while (getline(stream, field, delimiter)) {
        result.push_back(field);
    }
    return result;
}

// trim from start (inplace)
string& ltrimString(string& s,string words) {
	if (s.empty())
		return s;
	size_t index = s.find_first_not_of(words);
	if(index != std::string::npos)
		s.erase(0, index);
	return s;
}

// trim from end (inplace)
string& rtrimString(string& s, string words) {
	if (s.empty())
		return s;
	size_t index = s.find_last_not_of(words);
	if (index != std::string::npos)
		s.erase(index+1);
	return s;
}

// trim from both ends (inplace)
string& trimString(string& s, string words) {
    ltrimString(s,words);
    rtrimString(s, words);
	return s;
}

string replaceString(const string &sourceStr, string matchStr, string replaceStr) {
    string result=sourceStr;
    if(matchStr==replaceStr)
        return result;
    auto index=result.find(matchStr);
    while(index != string::npos){
        result.replace(index,matchStr.size(),replaceStr);
        index=result.find(matchStr,index+replaceStr.size());
    }
    return result;
}

int indexOfString(std::string s, std::string words)
{
	auto index = s.find_first_of(words);
	if (index == std::string::npos)
		return -1;
	return (int)index;
}

string& toLowerString(string& s) {
	for (char& c : s) {
		c=(char)tolower(static_cast<unsigned char>(c));
	}
	return s;
}

string GuidToString(const GUID& guid) {
	LPOLESTR wstr;
	StringFromCLSID(guid, &wstr);
	wstring ws(wstr);
	string result = WStringToString(ws);
	CoTaskMemFree(wstr);
	return result;
}

#pragma region 注册表相关
wstring readFromRegString(wstring name,wstring subkey,HKEY key)
{
	HKEY hKey;
	wchar_t value[MAX_PATH];
	DWORD dwBufferSize = sizeof(value);
	LONG lResult = RegOpenKeyExW(key, subkey.c_str(), 0, KEY_READ, &hKey);//打开节点
	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, name.c_str(), NULL, NULL, (LPBYTE)&value, &dwBufferSize);//读取配置
		if (lResult == ERROR_SUCCESS) {
			RegCloseKey(hKey);
			return wstring(value);
		}
	}
	RegCloseKey(hKey);
	return wstring();
}

int readFromRegWord(wstring name, wstring subkey, HKEY key)
{
	HKEY hKey;
	DWORD value;
	DWORD dwBufferSize = sizeof(value);
	LONG lResult = RegOpenKeyExW(key, subkey.c_str(), 0, KEY_READ, &hKey);//打开节点
	if (lResult == ERROR_SUCCESS) {
		lResult = RegQueryValueExW(hKey, name.c_str(), NULL, NULL, (LPBYTE)&value, &dwBufferSize);//读取配置
		if (lResult == ERROR_SUCCESS) {
			RegCloseKey(hKey);
			return static_cast<int>(value);
		}
	}
	RegCloseKey(hKey);
	return -1;

}
#pragma endregion