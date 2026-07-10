#include "IOHelper.h"

#include <filesystem>

#include "Utils.h"

const int readMaxSize = 64 * 1024;
using namespace std;
bool IOHelper::checkAndCreateDirectory(string path)
{
    return checkAndCreateDirectory(filesystem::path(path));
}

bool IOHelper::checkAndCreateDirectory(wstring path)
{
    return checkAndCreateDirectory(filesystem::path(path));
}

bool IOHelper::checkAndCreateDirectory(filesystem::path path) {
    try {
        if (!filesystem::exists(path)) {
            bool success = checkAndCreateDirectory(path.parent_path());//递归处理上级目录
            if (success) {
                return filesystem::create_directory(path); //只会创建1级的目录
            }
        }
        return true;
    } catch (filesystem::filesystem_error &e) {
        LOG_ERROR(u8"无法打开文件:%s", e.what());
    }
    return false;
}

bool IOHelper::exists(string path)
{
    return filesystem::exists(path);
}

string IOHelper::getFileNameWithoutExt(string filename)
{
    auto path = filesystem::path(filename);
    return getFileNameWithoutExt(path);
}

string IOHelper::getFileNameWithoutExt(filesystem::path path)
{
    if (path.filename().has_stem())
        return path.filename().stem().string();
    return path.filename().string();
}

string IOHelper::getFileNameExt(string filename)
{
    return getFileNameExt(filesystem::path(filename));
}

string IOHelper::getFileNameExt(filesystem::path path)
{
    return path.extension().string();
}

string IOHelper::pathCombine(string path1, string path2)
{
    if (!filesystem::is_directory(path1))//如果路径1已经是文件名，则返回文件名
        return path1;
    auto path= filesystem::path(path2);
    if (path.is_absolute()) { //路径2 已经是绝对路径
        return path2;
    }
    auto firstPath=filesystem::path(path1);
    auto resultPath = firstPath / path;
    return resultPath.string();
}


bool IOHelper::backupFile(string filename, string backupDirectory) {
    if (filesystem::exists(filename)) {
        try {
            auto path = filesystem::path(filename);
            auto ext = path.extension().string();
            string newFileName;
            for (int i = 0;; i++) {
                newFileName =pathCombine(backupDirectory , path.filename().stem().string() + "_" + getCurrentDate() + "_" + to_string(i) + ext);
                if (!filesystem::exists(newFileName))
                    break;
            }
            checkAndCreateDirectory(backupDirectory);
            filesystem::rename(filename, newFileName);
            return true;
        }
        catch (filesystem::filesystem_error& e) {
            //string content = u8"无法打开文件:" +  *e.what();
            //LOG_ERROR(content.c_str());
            LOG_ERROR(u8"无法打开文件:%s" , e.what());
        }
    }
    return false;
}

vector<string> IOHelper::readAllLine(string filename)
{
    vector<string> result;
    if (exists(filename)) {
        fstream fileStream;// 使用构造函数直接创建并打开文件
        fileStream.open(filename, ios::in); // 以只读模式打开
        if (!fileStream.is_open()) {
            string content = u8"无法打开文件:" + filename;
            LOG_ERROR(content.c_str());
            return result;
        }
        string line;
        while (getline(fileStream, line)) {
            result.push_back(line);
        }
    }else {
        string content = u8"读取的文件不存在:" + filename;
        LOG_WARN(content.c_str()); //这样写入没有问题
        //LOG_WARN(u8"读取的文件不存在:%s", &filename); //这样写入文件时，会乱码，且文件格式变成ansi
    }
    return result;
}

bool IOHelper::readEdIdData(const char* path, std::vector<char> vec)
{
    FILE* fp = NULL;
    if (fopen_s(&fp, path, "rb") == 0 && fp != NULL) {
        fseek(fp, 0, SEEK_END);
        auto file_len = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        bool ret = true;
        if (file_len > 0 && file_len <= readMaxSize)
        {
            std::vector<char> tmp(file_len);
            for (size_t total_len = 0; total_len < file_len; ) {
                auto read_len = fread(&tmp[total_len], 1, file_len - total_len, fp);
                if (read_len > 0) {
                    total_len += read_len;
                }
                else if (read_len == 0) {
                    break;
                }
                else {
                    LOG_ERROR("fread %s error: %d", path, read_len);
                    ret = false;
                    break;
                }
            }
            vec.swap(tmp);
        }
        else
        {
            LOG_ERROR("file %s too big: file_len=%d", path, file_len);
            ret = false;
        }
        fclose(fp);
        return ret;
    }
    else {
        LOG_INFO("fopen %s fail", path);
    }
    return false;
}
