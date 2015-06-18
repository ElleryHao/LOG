#ifndef _LOG_H
#define _LOG_H
#include<stdio.h>
#include<stdlib.h>
#include<assert.h>
#define STRING_LENGTH 1024
#define EE_SUCSESS 1
#define EE_FAILURE 0


static const char* LevelMsg[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
enum LogLevel
{
	TRACE,
	DEBUG,
	INFO,
	WARN,
	ERROR,
	FATAL
};

class Log
{
public:
	Log(const char* fileName);
	~Log();
private:	
	Log();
public:	
	int openFile(const char* fileName);
	void writeLog(LogLevel logLevel,const char* fmt,...);
	static Log* getInstance();
private:
	char m_fileName[STRING_LENGTH];
	FILE* m_fileStream;

};


#endif