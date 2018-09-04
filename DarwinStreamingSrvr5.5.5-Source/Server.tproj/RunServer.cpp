/*
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 *
 */
/*
    File:       main.cpp

    Contains:   main function to drive streaming server.

    

*/

#include <errno.h>

#include "RunServer.h"
#include "SafeStdLib.h"
#include "OS.h"
#include "OSMemory.h"
#include "OSThread.h"
#include "Socket.h"
#include "SocketUtils.h"
#include "ev.h"
#include "OSArrayObjectDeleter.h"
#include "Task.h"
#include "IdleTask.h"
#include "TimeoutTask.h"
#include "DateTranslator.h"
#include "QTSSRollingLog.h"


#ifndef __Win32__
    #include <sys/types.h>
    #include <unistd.h>
#endif
#include "QTSServerInterface.h"
#include "QTSServer.h"

#include <stdlib.h>
#include <sys/stat.h>

QTSServer* sServer = NULL;
int sStatusUpdateInterval = 0;
Bool16 sHasPID = false;
UInt64 sLastStatusPackets = 0;
UInt64 sLastDebugPackets = 0;
SInt64 sLastDebugTotalQuality = 0;
#ifdef __sgi__ 
#include <sched.h>
#endif
/*************************************************************************************************************
* 
*
*
**************************************************************************************************************/
QTSS_ServerState StartServer(XMLPrefsParser* inPrefsSource, PrefsSource* inMessagesSource, UInt16 inPortOverride, int statsUpdateInterval, QTSS_ServerState inInitialState, Bool16 inDontFork, UInt32 debugLevel, UInt32 debugOptions)
{
    //Mark when we are done starting up. If auto-restart is enabled, we want to make sure
    //to always exit with a status of 0 if we encountered a problem WHILE STARTING UP. This
    //will prevent infinite-auto-restart-loop type problems
    Bool16 doneStartingUp = false;
    QTSS_ServerState theServerState = qtssStartingUpState;
    
    sStatusUpdateInterval = statsUpdateInterval;
    
    //Initialize utility classes
    
    /* 获取系统时间(从1990年算起，经过的毫秒时间) */
    OS::Initialize();
    
    /* 初始化线程属性 */
    OSThread::Initialize();

	/* 创建事件线程 */
    Socket::Initialize();

    /* 获取下所有网络接口(接口)ip地址/dns 等 */
    SocketUtils::Initialize(!inDontFork);

	/* 初始化 select 事件调度环境 */
	/******************************************
	* 此处可修改为epoll
	* 其实在该流媒体系统，修改为epoll没有必要
	* 性能不会有明显的提升
	*******************************************/
#if !MACOSXEVENTQUEUE
    ::select_startevents();//initialize the select() implementation of the event queue
#endif

    /* 启动服务器 */
    //start the server

	/************************************************************
	* 
	* 初见系统相应模块的属性元数据映射结构
	*
	*************************************************************/
    QTSSDictionaryMap::Initialize();

	/************************************************************
	* 
	* 初始化QTSServer属性元数据(属性的属性)
	*
	*************************************************************/
    QTSServerInterface::Initialize();// this must be called before constructing the server object

	/// 创建服务器对象
	sServer = NEW QTSServer();

	/* 设置调试等级 */
    sServer->SetDebugLevel(debugLevel);

    /* 设置调试选项 */
    sServer->SetDebugOptions(debugOptions);

    /* 解析 streamingserver.xml 配置文件 */
    // re-parse config file
    inPrefsSource->Parse();

    Bool16 createListeners = true;
    if (qtssShuttingDownState == inInitialState) 
        createListeners = false;

	/* 服务器初始化 */
    sServer->Initialize(inPrefsSource, inMessagesSource, inPortOverride,createListeners);

    if (inInitialState == qtssShuttingDownState){  
        sServer->InitModules(inInitialState);
        return inInitialState;
    }

	/// 获取组名、用户名
    OSCharArrayDeleter runGroupName(sServer->GetPrefs()->GetRunGroupName());
    OSCharArrayDeleter runUserName(sServer->GetPrefs()->GetRunUserName());
    OSThread::SetPersonality(runUserName.GetObject(), runGroupName.GetObject());

    if (sServer->GetServerState() != qtssFatalErrorState){

		/* 探测cpu个数并创建与cpu个数相等的线程 */
        UInt32 numThreads = 0;
		
        /// 是否线程安全
        if (OS::ThreadSafe()){
            numThreads = sServer->GetPrefs()->GetNumThreads(); // whatever the prefs say
            if (numThreads == 0)

				/* 获取cpu核心数 */
                numThreads = OS::GetNumProcessors(); // 1 worker thread per processor
        }

		/* 获取失败，则只生成一个任务线程 */
        if (numThreads == 0)
            numThreads = 1;

		/* 项任务线程池中添加指定个数的任务线程 */
        TaskThreadPool::AddThreads(numThreads);
		
    #if DEBUG
        qtss_printf("Number of task threads: %lu\n",numThreads);
    #endif

    	/* 初始化超时任务 */
        // Start up the server's global tasks, and start listening
        TimeoutTask::Initialize();     // The TimeoutTask mechanism is task based,
                                    // we therefore must do this after adding task threads
                                    // this be done before starting the sockets and server tasks
     }

    //Make sure to do this stuff last. Because these are all the threads that
    //do work in the server, this ensures that no work can go on while the server
    //is in the process of staring up
    if (sServer->GetServerState() != qtssFatalErrorState){

		/* 创建调度任务线程 */
        IdleTask::Initialize();
		
        /* 启动事件线程 */
        Socket::StartThread();
		
        /* 睡眠1S 等待线程池的启动? */
        OSThread::Sleep(1000);
        
        //
        // On Win32, in order to call modwatch the Socket EventQueue thread must be
        // created first. Modules call modwatch from their initializer, and we don't
        // want to prevent them from doing that, so module initialization is separated
        // out from other initialization, and we start the Socket EventQueue thread first.
        // The server is still prevented from doing anything as of yet, because there
        // aren't any TaskThreads yet.

		/***************************************************************************************
		*
		* 加载并初始化模块
		* 初始化模块是会调用QTSS_Register_Role及QTSS_Initialize_Role，向系统注册模块全局属性
		* 
		*****************************************************************************************/
        sServer->InitModules(inInitialState);
		
        /* 创建 rtcp 任务 ，状态监测任务，并启动tcp监听任务*/
        sServer->StartTasks();
		
		/* 创建 rtp/rtcp Socket */        
        sServer->SetupUDPSockets(); // udp sockets are set up after the rtcp task is instantiated

		/// 获取服务器状态
		theServerState = sServer->GetServerState();
    }

    if (theServerState != qtssFatalErrorState){	
        CleanPid(true);
        WritePid(!inDontFork);

        doneStartingUp = true;
        qtss_printf("Streaming Server done starting up\n");
        OSMemory::SetMemoryError(ENOMEM);
    }

	/* 切换到普通用户运行 */
    // SWITCH TO RUN USER AND GROUP ID
    if (!sServer->SwitchPersonality())
        theServerState = qtssFatalErrorState;

   //
    // Tell the caller whether the server started up or not
    return theServerState;
}

void WritePid(Bool16 forked)
{
#ifndef __Win32__
    // WRITE PID TO FILE
    OSCharArrayDeleter thePidFileName(sServer->GetPrefs()->GetPidFilePath());
    FILE *thePidFile = fopen(thePidFileName, "w");
    if(thePidFile)
    {
        if (!forked)
            fprintf(thePidFile,"%d\n",getpid());    // write own pid
        else
        {
            fprintf(thePidFile,"%d\n",getppid());    // write parent pid
            fprintf(thePidFile,"%d\n",getpid());    // and our own pid in the next line
        }                
        fclose(thePidFile);
        sHasPID = true;
    }
#endif
}

void CleanPid(Bool16 force)
{
#ifndef __Win32__
    if (sHasPID || force)
    {
        OSCharArrayDeleter thePidFileName(sServer->GetPrefs()->GetPidFilePath());
        unlink(thePidFileName);
    }
#endif
}
void LogStatus(QTSS_ServerState theServerState)
{	

    static QTSS_ServerState lastServerState = 0;
    static char *sPLISTHeader[] =
    {     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
#if __MacOSX__
        "<!DOCTYPE plist SYSTEM \"file://localhost/System/Library/DTDs/PropertyList.dtd\">",
#else
        "<!ENTITY % plistObject \"(array | data | date | dict | real | integer | string | true | false )\">",
        "<!ELEMENT plist %plistObject;>",
        "<!ATTLIST plist version CDATA \"0.9\">",
        "",
        "<!-- Collections -->",
        "<!ELEMENT array (%plistObject;)*>",
        "<!ELEMENT dict (key, %plistObject;)*>",
        "<!ELEMENT key (#PCDATA)>",
        "",
        "<!--- Primitive types -->",
        "<!ELEMENT string (#PCDATA)>",
        "<!ELEMENT data (#PCDATA)> <!-- Contents interpreted as Base-64 encoded -->",
        "<!ELEMENT date (#PCDATA)> <!-- Contents should conform to a subset of ISO 8601 (in particular, YYYY '-' MM '-' DD 'T' HH ':' MM ':' SS 'Z'.  Smaller units may be omitted with a loss of precision) -->",
        "",
        "<!-- Numerical primitives -->",
        "<!ELEMENT true EMPTY>  <!-- Boolean constant true -->",
        "<!ELEMENT false EMPTY> <!-- Boolean constant false -->",
        "<!ELEMENT real (#PCDATA)> <!-- Contents should represent a floating point number matching (\"+\" | \"-\")? d+ (\".\"d*)? (\"E\" (\"+\" | \"-\") d+)? where d is a digit 0-9.  -->",
        "<!ELEMENT integer (#PCDATA)> <!-- Contents should represent a (possibly signed) integer number in base 10 -->",
        "]>",
#endif
    };

    static int numHeaderLines = sizeof(sPLISTHeader) / sizeof(char*);

    static char*    sPlistStart = "<plist version=\"0.9\">";
    static char*    sPlistEnd = "</plist>";
    static char*    sDictStart = "<dict>";
    static char*    sDictEnd = "</dict>";
    
    static char*    sKey    = "     <key>%s</key>\n";
    static char*    sValue  = "     <string>%s</string>\n";
    
    static char *sAttributes[] =
    {
        "qtssSvrServerName",
        "qtssSvrServerVersion",
        "qtssSvrServerBuild",
        "qtssSvrServerPlatform",
        "qtssSvrRTSPServerComment",
        "qtssSvrServerBuildDate",
        "qtssSvrStartupTime",
        "qtssSvrCurrentTimeMilliseconds",
        "qtssSvrCPULoadPercent",
         "qtssSvrState",
        "qtssRTPSvrCurConn",
        "qtssRTSPCurrentSessionCount",
        "qtssRTSPHTTPCurrentSessionCount",
        "qtssRTPSvrCurBandwidth",
        "qtssRTPSvrCurPackets",
        "qtssRTPSvrTotalConn",
        "qtssRTPSvrTotalBytes",
        "qtssMP3SvrCurConn",
        "qtssMP3SvrTotalConn",
        "qtssMP3SvrCurBandwidth",
        "qtssMP3SvrTotalBytes"
    };
    static int numAttributes = sizeof(sAttributes) / sizeof(char*);
        
    static StrPtrLen statsFileNameStr("server_status");    

    /*检查是否启动创建 server_status 文件功能*/
    if (false == sServer->GetPrefs()->ServerStatFileEnabled())
        return;
    /* 检查写文件间隔,如果为0 则不写 server_status 文件，返回 */
    UInt32 interval = sServer->GetPrefs()->GetStatFileIntervalSec();
    if (interval == 0 || (OS::UnixTime_Secs() % interval) > 0 ){ 
        return;
	}
        
    /* 检查RTSP Session 是否为 0 ，如果为0，没有必要写 server_status 文件  */
    // If the total number of RTSP sessions is 0  then we 
    // might not need to update the "server_status" file.
    char* thePrefStr = NULL;
    // We start lastRTSPSessionCount off with an impossible value so that
    // we force the "server_status" file to be written at least once.
    static int lastRTSPSessionCount = -1; 
    // Get the RTSP session count from the server.
    (void)QTSS_GetValueAsString(sServer, qtssRTSPCurrentSessionCount, 0, &thePrefStr);
    int currentRTSPSessionCount = ::atoi(thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;
    if (currentRTSPSessionCount == 0 && currentRTSPSessionCount == lastRTSPSessionCount){
        // we don't need to update the "server_status" file except the
        // first time we are in the idle state.
        if (theServerState == qtssIdleState && lastServerState == qtssIdleState){
            lastRTSPSessionCount = currentRTSPSessionCount;
            lastServerState = theServerState;
            return;
        }
    }else{
        // save the RTSP session count for the next time we execute.
        lastRTSPSessionCount = currentRTSPSessionCount;
    }

	/* 必须要创建 server_status 文件，首先获取文件路径 并打开文件 */
    StrPtrLenDel pathStr(sServer->GetPrefs()->GetErrorLogDir());
    StrPtrLenDel fileNameStr(sServer->GetPrefs()->GetStatsMonitorFileName());
    ResizeableStringFormatter pathBuffer(NULL,0);
    pathBuffer.PutFilePath(&pathStr,&fileNameStr);
    pathBuffer.PutTerminator();
    
    char*   filePath = pathBuffer.GetBufPtr();   
    /* filePath = /var/streaming/logs/server_status */
    FILE*   statusFile = ::fopen(filePath, "w");
    char*   theAttributeValue = NULL;
    int     i;
    
    if (statusFile != NULL){	
    	/* 打开文件成功，修改权限 */
        ::chmod(filePath, 0640);

		/* 开始写入状态信息 */
        for ( i = 0; i < numHeaderLines; i++){    
            qtss_fprintf(statusFile, "%s\n",sPLISTHeader[i]);    
        }
		
        qtss_fprintf(statusFile, "%s\n", sPlistStart);
        qtss_fprintf(statusFile, "%s\n", sDictStart);    

          // show each element value
         for ( i = 0; i < numAttributes; i++){
            (void)QTSS_GetValueAsString(sServer, QTSSModuleUtils::GetAttrID(sServer,sAttributes[i]), 0, &theAttributeValue);
            if (theAttributeValue != NULL){
                qtss_fprintf(statusFile, sKey, sAttributes[i]);    
               qtss_fprintf(statusFile, sValue, theAttributeValue);    
                delete [] theAttributeValue;
                theAttributeValue = NULL;
             }
         }
                  
        qtss_fprintf(statusFile, "%s\n", sDictEnd);
        qtss_fprintf(statusFile, "%s\n\n", sPlistEnd);    

		/* 关闭文件 */
        ::fclose(statusFile);
    }

	/* 记录服务器最后状态 */
    lastServerState = theServerState;
}

void print_status(FILE* file, FILE* console, char* format, char* theStr)
{
    if (file) qtss_fprintf(file, format, theStr);
    if (console) qtss_fprintf(console, format, theStr);

}

void DebugLevel_1(FILE*   statusFile, FILE*   stdOut,  Bool16 printHeader )
{
    char*  thePrefStr = NULL;
    static char numStr[12] ="";
    static char dateStr[25] ="";
    UInt32 theLen = 0;

    if ( printHeader )
    {                   
   
        print_status(statusFile,stdOut,"%s", "     RTP-Conns RTSP-Conns HTTP-Conns  kBits/Sec   Pkts/Sec   RTP-Playing   AvgDelay CurMaxDelay  MaxDelay  AvgQuality  NumThinned      Time\n");

    }
    
    (void)QTSS_GetValueAsString(sServer, qtssRTPSvrCurConn, 0, &thePrefStr);
    print_status(statusFile, stdOut,"%11s", thePrefStr);
    
    delete [] thePrefStr; thePrefStr = NULL;
    
    (void)QTSS_GetValueAsString(sServer, qtssRTSPCurrentSessionCount, 0, &thePrefStr);
    print_status(statusFile, stdOut,"%11s", thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;
    
    (void)QTSS_GetValueAsString(sServer, qtssRTSPHTTPCurrentSessionCount, 0, &thePrefStr);
    print_status(statusFile, stdOut,"%11s", thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;
    
    UInt32 curBandwidth = 0;
    theLen = sizeof(curBandwidth);
    (void)QTSS_GetValue(sServer, qtssRTPSvrCurBandwidth, 0, &curBandwidth, &theLen);
    qtss_snprintf(numStr, 11, "%lu", curBandwidth/1024);
    print_status(statusFile, stdOut,"%11s", numStr);

    (void)QTSS_GetValueAsString(sServer, qtssRTPSvrCurPackets, 0, &thePrefStr);
    print_status(statusFile, stdOut,"%11s", thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;


    UInt32 currentPlaying = sServer->GetNumRTPPlayingSessions();
    qtss_snprintf( numStr, sizeof(numStr) -1, "%lu", currentPlaying);
    print_status(statusFile, stdOut,"%14s", numStr);

   
    //is the server keeping up with the streams?
    //what quality are the streams?
    SInt64 totalRTPPaackets = sServer->GetTotalRTPPackets();
    SInt64 deltaPackets = totalRTPPaackets - sLastDebugPackets;
    sLastDebugPackets = totalRTPPaackets;

    SInt64 totalQuality = sServer->GetTotalQuality();
    SInt64 deltaQuality = totalQuality - sLastDebugTotalQuality;
    sLastDebugTotalQuality = totalQuality;

    SInt64 currentMaxLate =  sServer->GetCurrentMaxLate();
    SInt64 totalLate =  sServer->GetTotalLate();

    sServer->ClearTotalLate();
    sServer->ClearCurrentMaxLate();
    sServer->ClearTotalQuality();
    
    ::qtss_snprintf(numStr, sizeof(numStr) -1, "%s", "0");
    if (deltaPackets > 0)
        qtss_snprintf(numStr, sizeof(numStr) -1, "%ld", (SInt32) ((SInt64)totalLate /  (SInt64) deltaPackets ));
    print_status(statusFile, stdOut,"%11s", numStr);

    qtss_snprintf(numStr,sizeof(numStr) -1, "%ld", (SInt32) currentMaxLate);
    print_status(statusFile, stdOut,"%11s", numStr);
    
    qtss_snprintf(numStr,sizeof(numStr) -1, "%ld", (SInt32)  sServer->GetMaxLate() );
    print_status(statusFile, stdOut,"%11s", numStr);

    ::qtss_snprintf(numStr, sizeof(numStr) -1, "%s", "0");
    if (deltaPackets > 0)
        qtss_snprintf(numStr, sizeof(numStr) -1, "%ld", (SInt32) ((SInt64) deltaQuality /  (SInt64) deltaPackets));
    print_status(statusFile, stdOut,"%11s", numStr);

    qtss_snprintf(numStr,sizeof(numStr) -1, "%ld", (SInt32) sServer->GetNumThinned() );
    print_status(statusFile, stdOut,"%11s", numStr);

    
    
    char theDateBuffer[QTSSRollingLog::kMaxDateBufferSizeInBytes];
    (void) QTSSRollingLog::FormatDate(theDateBuffer, false);
    
    qtss_snprintf(dateStr,sizeof(dateStr) -1, "%s", theDateBuffer );
    print_status(statusFile, stdOut,"%24s\n", dateStr);
}

FILE* LogDebugEnabled()
{

    if (DebugLogOn(sServer))
    {
        static StrPtrLen statsFileNameStr("server_debug_status");    
    
        StrPtrLenDel pathStr(sServer->GetPrefs()->GetErrorLogDir());
        ResizeableStringFormatter pathBuffer(NULL,0);
        pathBuffer.PutFilePath(&pathStr,&statsFileNameStr);
        pathBuffer.PutTerminator();
        
        char*   filePath = pathBuffer.GetBufPtr();    
        return ::fopen(filePath, "a");
    }
    
    return NULL;
}


FILE* DisplayDebugEnabled()
{        
    return ( DebugDisplayOn(sServer) ) ? stdout   : NULL ;
}


void DebugStatus(UInt32 debugLevel, Bool16 printHeader)
{
        
    FILE*   statusFile = LogDebugEnabled();
    FILE*   stdOut = DisplayDebugEnabled();
    
    if (debugLevel > 0)
        DebugLevel_1(statusFile, stdOut, printHeader);

    if (statusFile) 
        ::fclose(statusFile);
}

void FormattedTotalBytesBuffer(char *outBuffer, int outBufferLen, UInt64 totalBytes)
{
    Float32 displayBytes = 0.0;
    char  sizeStr[] = "B";
    char* format = NULL;
        
    if (totalBytes > 1073741824 ) //GBytes
    {   displayBytes = (Float32) ( (Float64) (SInt64) totalBytes /  (Float64) (SInt64) 1073741824 );
        sizeStr[0] = 'G';
        format = "%.4f%s ";
     }
    else if (totalBytes > 1048576 ) //MBytes
    {   displayBytes = (Float32) (SInt32) totalBytes /  (Float32) (SInt32) 1048576;
        sizeStr[0] = 'M';
        format = "%.3f%s ";
     }
    else if (totalBytes > 1024 ) //KBytes
    {    displayBytes = (Float32) (SInt32) totalBytes /  (Float32) (SInt32) 1024;
         sizeStr[0] = 'K';
         format = "%.2f%s ";
    }
    else
    {    displayBytes = (Float32) (SInt32) totalBytes;  //Bytes
         sizeStr[0] = 'B';
         format = "%4.0f%s ";
    }
    
    outBuffer[outBufferLen -1] = 0;
    qtss_snprintf(outBuffer, outBufferLen -1,  format , displayBytes, sizeStr);
}

void PrintStatus(Bool16 printHeader)
{
    char* thePrefStr = NULL;
    UInt32 theLen = 0;
    
    if ( printHeader )
    {                       
        qtss_printf("     RTP-Conns RTSP-Conns HTTP-Conns  kBits/Sec   Pkts/Sec    TotConn     TotBytes   TotPktsLost          Time\n");   
    }

    (void)QTSS_GetValueAsString(sServer, qtssRTPSvrCurConn, 0, &thePrefStr);
    qtss_printf( "%11s", thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;
    
    (void)QTSS_GetValueAsString(sServer, qtssRTSPCurrentSessionCount, 0, &thePrefStr);
    qtss_printf( "%11s", thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;
    
    (void)QTSS_GetValueAsString(sServer, qtssRTSPHTTPCurrentSessionCount, 0, &thePrefStr);
    qtss_printf( "%11s", thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;
    
    UInt32 curBandwidth = 0;
    theLen = sizeof(curBandwidth);
    (void)QTSS_GetValue(sServer, qtssRTPSvrCurBandwidth, 0, &curBandwidth, &theLen);
    qtss_printf( "%11lu", curBandwidth/1024);
    
    (void)QTSS_GetValueAsString(sServer, qtssRTPSvrCurPackets, 0, &thePrefStr);
    qtss_printf( "%11s", thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;
    
    (void)QTSS_GetValueAsString(sServer, qtssRTPSvrTotalConn, 0, &thePrefStr);
    qtss_printf( "%11s", thePrefStr);
    delete [] thePrefStr; thePrefStr = NULL;
    
    UInt64 totalBytes = sServer->GetTotalRTPBytes();
    char  displayBuff[32] = "";
    FormattedTotalBytesBuffer(displayBuff, sizeof(displayBuff),totalBytes);
    qtss_printf( "%17s", displayBuff);
    
    qtss_printf( "%11"_64BITARG_"u", sServer->GetTotalRTPPacketsLost());
                    
    char theDateBuffer[QTSSRollingLog::kMaxDateBufferSizeInBytes];
    (void) QTSSRollingLog::FormatDate(theDateBuffer, false);
    qtss_printf( "%25s",theDateBuffer);
    
    qtss_printf( "\n");
    
}

Bool16 PrintHeader(UInt32 loopCount)
{
     return ( (loopCount % (sStatusUpdateInterval * 10) ) == 0 ) ? true : false;
}

Bool16 PrintLine(UInt32 loopCount)
{
     return ( (loopCount % sStatusUpdateInterval) == 0 ) ? true : false;
}


void RunServer()
{   
    Bool16 restartServer = false;
    UInt32 loopCount = 0;
    UInt32 debugLevel = 0;
    Bool16 printHeader = false;
    Bool16 printStatus = false;

	/* 
		进程每隔1S检查一次系统状态 
		并等待进程被人为停止或发生意外情况
	*/
    //just wait until someone stops the server or a fatal error occurs.
    QTSS_ServerState theServerState = sServer->GetServerState();
    while ((theServerState != qtssShuttingDownState) &&
            (theServerState != qtssFatalErrorState))
    {
    	/* 
    		睡眠1s 
    	*/
#ifdef __sgi__
        OSThread::Sleep(999);
#else
        OSThread::Sleep(1000);
#endif
		/* 记录服务器状态信息 */
        LogStatus(theServerState);

		/*
			检查是否定义了更新间隔
		*/
        if (sStatusUpdateInterval)
        {
            debugLevel = sServer->GetDebugLevel();             
            printHeader = PrintHeader(loopCount);
            printStatus = PrintLine(loopCount);
                
            if (printStatus)
            {
                if  (DebugOn(sServer) ) // debug level display or logging is on
                    DebugStatus(debugLevel, printHeader);
                
                if (!DebugDisplayOn(sServer))
                    PrintStatus(printHeader); // default status output
            }
            
            
            loopCount++;

        }
		
        /* 服务器收到 SIGINT 信号或 SIGTERM 信号  */
        if ((sServer->SigIntSet()) || (sServer->SigTermSet()))
        {
            /* 关闭服务进程 */
            // start the shutdown process
            theServerState = qtssShuttingDownState;
            (void)QTSS_SetValue(QTSServerInterface::GetServer(), qtssSvrState, 0, &theServerState, sizeof(theServerState));
			/* 收到 SIGINT 信号 重启服务器 */
            if (sServer->SigIntSet())
                restartServer = true;
        }
        
        theServerState = sServer->GetServerState();
        /* 关闭所有的 RTP Session */
        if (theServerState == qtssIdleState)
            sServer->KillAllRTPSessions();
    }

    /* 服务进程 收到退出状态或严重异常错误状态，准备退出 */
    
    //
    // Kill all the sessions and wait for them to die,
    // but don't wait more than 5 seconds
    
    /* 杀死所有RTP Session */
    sServer->KillAllRTPSessions();
    for (UInt32 shutdownWaitCount = 0; (sServer->GetNumRTPSessions() > 0) && (shutdownWaitCount < 5); shutdownWaitCount++)
        OSThread::Sleep(1000);
    
    /* 清空线程池 */
    //Now, make sure that the server can't do any work
    TaskThreadPool::RemoveThreads();
    
    //now that the server is definitely stopped, it is safe to initate
    //the shutdown process
    delete sServer;
    
    /* 清理进程号 */
    CleanPid(false);
    //ok, we're ready to exit. If we're quitting because of some fatal error
    //while running the server, make sure to let the parent process know by
    //exiting with a nonzero status. Otherwise, exit with a 0 status
    if (theServerState == qtssFatalErrorState || restartServer)
        ::exit (-2);//-2 signals parent process to restart server
}
