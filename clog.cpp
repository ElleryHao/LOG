#include "clog.h"
#include<string>
#include <stdarg.h>
#include<time.h>
//author hzkai
#define DateTimeFormat "%Y-%m-%d %H:%M:%S"


Log::Log()
{
	memset(m_fileName,0,STRING_LENGTH);
	m_fileStream=NULL;
}

Log::Log(const char* fileName)
{
	memset(m_fileName,0,STRING_LENGTH);
	m_fileStream=fopen(fileName,"ab+");
    
	assert(NULL != m_fileStream);
}

Log* Log::getInstance()
{
	return new Log();
}

int Log::openFile(const char* fileName)
{
	FILE* temp=fopen(fileName,"ab+");
	m_fileStream= temp?temp:m_fileStream;
	if(NULL == temp)
	{
		return EE_FAILURE;
	}
	int len=strlen(fileName);
	strncpy(m_fileName,fileName,len);
	return EE_SUCSESS;
}
Log::~Log()
{
	fclose(m_fileStream);
	memset(m_fileName,0,STRING_LENGTH);
}
void Log::writeLog(LogLevel logLevel,const char* fmt,...)
{
	char temp[STRING_LENGTH]={0};
	va_list ap;
	va_start(ap,fmt);
	vsnprintf(temp,sizeof(temp),fmt,ap);
	va_end(ap);
	time_t now = time(NULL);
	char buf[STRING_LENGTH]={0};
  strftime(buf, sizeof(buf), DateTimeFormat, gmtime(&now));
	fprintf(m_fileStream,"%s %s | %s\r\n",buf,LevelMsg[logLevel],temp);
}