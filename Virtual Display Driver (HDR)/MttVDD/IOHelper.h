#pragma once

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
class IOHelper
{
public:
	static bool checkAndCreateDirectory(std::string path);
    static bool checkAndCreateDirectory(std::wstring path);
	static bool checkAndCreateDirectory(std::filesystem::path path);
	static bool exists(std::string path);
	static std::string getFileNameWithoutExt(std::string filename);
	static std::string getFileNameWithoutExt(std::filesystem::path path);
	static std::string getFileNameExt(std::string filename);
	static std::string getFileNameExt(std::filesystem::path path);
	static std::string pathCombine(std::string path1, std::string path2);
	static bool backupFile(std::string filename, std::string backupDirectory);

	static std::vector<std::string> readAllLine(std::string filename);
    static bool readEdIdData(const char* path,std::vector<char> vec);
};

