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
    File:       QTSServer.cpp

    Contains:   Implements object defined in QTSServer.h
    
    

*/


#ifndef __Win32__
#include <sys/types.h>
#include <dirent.h>
#endif
#include <errno.h>

#ifndef __Win32__
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#endif

#include "QTSServer.h"

#include "OSMemory.h"
#include "OSArrayObjectDeleter.h"
#include "SocketUtils.h"
#include "TCPListenerSocket.h"
#include "Task.h"

#include "QTSS_Private.h"
#include "QTSSCallbacks.h"
#include "QTSSModuleUtils.h"

//Compile time modules
#include "QTSSErrorLogModule.h"
#include "QTSSFileModule.h"
#include "QTSSAccessLogModule.h"
#include "QTSSFlowControlModule.h"
#include "QTSSReflectorModule.h"
#ifdef PROXYSERVER
#include "QTSSProxyModule.h"
#endif
#include "QTSSRelayModule.h"
#include "QTSSPosixFileSysModule.h"
#include "QTSSAdminModule.h"
#include "QTSSAccessModule.h"
#include "QTSSMP3StreamingModule.h"
#if MEMORY_DEBUGGING
#include "QTSSWebDebugModule.h"
#endif



#include "RTSPRequestInterface.h"
#include "RTSPSessionInterface.h"
#include "RTPSessionInterface.h"
#include "RTSPSession.h"
#include "RTPStream.h"
#include "RTCPTask.h"
#include "QTSSFile.h"

// CLASS DEFINITIONS

class RTSPListenerSocket : public TCPListenerSocket
{
    public:
    
        RTSPListenerSocket() {}
        virtual ~RTSPListenerSocket() {}
        
        //sole job of this object is to implement this function
        virtual Task*   GetSessionTask(TCPSocket** outSocket);
        
        //check whether the Listener should be idling
        Bool16 OverMaxConnections(UInt32 buffer);

};

class RTPSocketPool : public UDPSocketPool
{
    public:
    
        // Pool of UDP sockets for use by the RTP server
        
        RTPSocketPool() {}
        ~RTPSocketPool() {}
        
        virtual UDPSocketPair*  ConstructUDPSocketPair();
        virtual void            DestructUDPSocketPair(UDPSocketPair* inPair);

        virtual void            SetUDPSocketOptions(UDPSocketPair* inPair);
};



char*           QTSServer::sPortPrefString = "rtsp_port";
QTSS_Callbacks  QTSServer::sCallbacks;
XMLPrefsParser* QTSServer::sPrefsSource = NULL;
PrefsSource*    QTSServer::sMessagesSource = NULL;


QTSServer::~QTSServer()
{
    //
    // Grab the server mutex. This is to make sure all gets & set values on this
    // object complete before we start deleting stuff
    OSMutexLocker serverlocker(this->GetServerObjectMutex());
    
    //
    // Grab the prefs mutex. This is to make sure we can't reread prefs
    // WHILE shutting down, which would cause some weirdness for QTSS API
    // (some modules could get QTSS_RereadPrefs_Role after QTSS_Shutdown, which would be bad)
    OSMutexLocker locker(this->GetPrefs()->GetMutex());

    QTSS_ModuleState theModuleState;
    theModuleState.curRole = QTSS_Shutdown_Role;
    theModuleState.curTask = NULL;
    OSThread::SetMainThreadData(&theModuleState);

    for (UInt32 x = 0; x < QTSServerInterface::GetNumModulesInRole(QTSSModule::kShutdownRole); x++)
        (void)QTSServerInterface::GetModule(QTSSModule::kShutdownRole, x)->CallDispatch(QTSS_Shutdown_Role, NULL);

    OSThread::SetMainThreadData(NULL);
}

Bool16 QTSServer::Initialize(XMLPrefsParser* inPrefsSource, PrefsSource* inMessagesSource, UInt16 inPortOverride, Bool16 createListeners)
{
    static const UInt32 kRTPSessionMapSize = 577;
    fServerState = qtssFatalErrorState;
    sPrefsSource = inPrefsSource;
    sMessagesSource = inMessagesSource;

    /* ��ʼ���ص����� */
    this->InitCallbacks();

	/************************************************************
	* 
	* ��ʼ�� QTSSModule ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    QTSSModule::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� QTSServerPrefs ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    QTSServerPrefs::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� QTSSMessages ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    QTSSMessages::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� RTSPRequestInterface ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    RTSPRequestInterface::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� RTSPSessionInterface ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    RTSPSessionInterface::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� RTPSessionInterface ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    RTPSessionInterface::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� RTPStream ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    RTPStream::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� RTSPSession ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    RTSPSession::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� QTSSFile ����Ԫ����(���Ե�����)
	*
	*************************************************************/
    QTSSFile::Initialize();
    
	/************************************************************
	* 
	* ��ʼ�� QTSSUserProfile ����Ԫ����(���Ե�����)
	*
	*************************************************************/

    QTSSUserProfile::Initialize();
    
    //
    // STUB SERVER INITIALIZATION
    //
    // Construct stub versions of the prefs and messages dictionaries. We need
    // both of these to initialize the server, but they have to be stubs because
    // their QTSSDictionaryMaps will presumably be modified when modules get loaded.

    /* ����������ƫ������ */
    fSrvrPrefs = new QTSServerPrefs(inPrefsSource, false); // First time, don't write changes to the prefs file

	/* ������Ϣ�� */
    fSrvrMessages = new QTSSMessages(inMessagesSource);

	/* ��ʼ��ģ�鹤���� */
    QTSSModuleUtils::Initialize(fSrvrMessages, this, QTSServerInterface::GetErrorLogStream());

    //
    // SETUP ASSERT BEHAVIOR
    //
    // Depending on the server preference, we will either break when we hit an
    // assert, or log the assert to the error log
	
    /* �����쳣��Ϊ */
    if (!fSrvrPrefs->ShouldServerBreakOnAssert()){
        SetAssertLogger(this->GetErrorLogStream());// the error log stream is our assert logger
    }
	
    //
    // CREATE GLOBAL OBJECTS

    /* ����ȫ�ֶ��� */
    fSocketPool = new RTPSocketPool();

	/* ����RTPSession ���ñ� */
    fRTPMap = new OSRefTable(kRTPSessionMapSize);

    //
    // Load ERROR LOG module only. This is good in case there is a startup error.

    /* ���� QTSSErrorLogModule ģ�� */
    QTSSModule* theLoggingModule = new QTSSModule("QTSSErrorLogModule");
    (void)theLoggingModule->SetupModule(&sCallbacks, &QTSSErrorLogModule_Main);
    (void)AddModule(theLoggingModule);
    this->BuildModuleRoleArrays();

    /* ����Ĭ��ip�� dns  */
    // DEFAULT IP ADDRESS & DNS NAME
    if (!this->SetDefaultIPAddr()){
        return false;
    }

    /* ��¼����ʱ�� */
    // STARTUP TIME - record it
    fStartupTime_UnixMilli = OS::Milliseconds();
    fGMTOffset = OS::GetGMTOffset();
        
    /* �������� */
    // BEGIN LISTENING
    if (createListeners){
        if ( !this->CreateListeners(false/* false ֻ���������� */, fSrvrPrefs, inPortOverride) ){
            QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssMsgSomePortsFailed, 0);
		}
    }
    
    if ( fNumListeners == 0 ){   
		if (createListeners){
            QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssMsgNoPortsSucceeded, 0);
		}
        return false;
    }

    /* �����ɹ� */
    fServerState = qtssStartingUpState;
    return true;
}

void QTSServer::InitModules(QTSS_ServerState inEndState)
{
    //
    // LOAD AND INITIALIZE ALL MODULES
        
    // temporarily set the verbosity on missing prefs when starting up to debug level
    // This keeps all the pref messages being written to the config file from being logged.
    // don't exit until the verbosity level is reset back to the initial prefs.

	/* �����ⲿģ�� */
    LoadModules(fSrvrPrefs);
	
	/* �����ڲ�ģ�� */
    LoadCompiledInModules();
	
	/* �ؽ�ģ���ɫ���� */
    this->BuildModuleRoleArrays();

	/// �ڳ�ʼ������ģ��ʱ�ر���Ϣ��Ϣ
    fSrvrPrefs->SetErrorLogVerbosity(qtssWarningVerbosity); // turn off info messages while initializing compiled in modules.
   //
    // CREATE MODULE OBJECTS AND READ IN MODULE PREFS
    
    // Finish setting up modules. Create our final prefs & messages objects,
    // register all global dictionaries, and invoke the modules in their Init roles.
    fStubSrvrPrefs = fSrvrPrefs;			
    fStubSrvrMessages = fSrvrMessages;

    fSrvrPrefs = new QTSServerPrefs(sPrefsSource, true); // Now write changes to the prefs file. First time, we don't because the error messages won't get printed.
    QTSS_ErrorVerbosity serverLevel = fSrvrPrefs->GetErrorLogVerbosity(); // get the real prefs verbosity and save it.
    fSrvrPrefs->SetErrorLogVerbosity(qtssWarningVerbosity); // turn off info messages while loading dynamic modules
   
 
    fSrvrMessages = new QTSSMessages(sMessagesSource);
    QTSSModuleUtils::Initialize(fSrvrMessages, this, QTSServerInterface::GetErrorLogStream());

    this->SetVal(qtssSvrMessages, &fSrvrMessages, sizeof(fSrvrMessages));
    this->SetVal(qtssSvrPreferences, &fSrvrPrefs, sizeof(fSrvrPrefs));

    //
    // ADD REREAD PREFERENCES SERVICE
    (void)QTSSDictionaryMap::GetMap(QTSSDictionaryMap::kServiceDictIndex)->AddAttribute(
    													QTSS_REREAD_PREFS_SERVICE, 
    													(QTSS_AttrFunctionPtr)QTSServer::RereadPrefsService, 
    													qtssAttrDataTypeUnknown, 
    													qtssAttrModeRead);

    //
    // INVOKE INITIALIZE ROLE
    this->DoInitRole();

    if (fServerState != qtssFatalErrorState){
        fServerState = inEndState; // Server is done starting up!   
    }


    fSrvrPrefs->SetErrorLogVerbosity(serverLevel); // reset the server's verbosity back to the original prefs level.
}

void QTSServer::StartTasks()
{
	/* ���� RTCPTask ���� ������*/
    fRTCPTask = new RTCPTask();
	/* ���� RTPStatsUpdaterTask �������� */
    fStatsTask = new RTPStatsUpdaterTask();

    /* �������� */
    // Start listening
    for (UInt32 x = 0; x < fNumListeners; x++)
        fListeners[x]->RequestEvent(EV_RE);
}

Bool16 QTSServer::SetDefaultIPAddr()
{
	/* ��飬��ȷ����һ�����õ�IP�ӿ� */
    //check to make sure there is an available ip interface
    if (SocketUtils::GetNumIPAddrs() == 0)
    {
        QTSSModuleUtils::LogError(qtssFatalVerbosity, qtssMsgNotConfiguredForIP, 0);
        return false;
    }

	/* �ҳ����ǵ�Ĭ��IP��ַ������ */
    //find out what our default IP addr is & dns name
    UInt32 theNumAddrs = 0;
    UInt32* theIPAddrs = this->GetRTSPIPAddrs(fSrvrPrefs, &theNumAddrs);
    if (theNumAddrs == 1)
        fDefaultIPAddr = SocketUtils::GetIPAddr(0);
    else
        fDefaultIPAddr = theIPAddrs[0];
    delete [] theIPAddrs;
        
    for (UInt32 ipAddrIter = 0; ipAddrIter < SocketUtils::GetNumIPAddrs(); ipAddrIter++)
    {
        if (SocketUtils::GetIPAddr(ipAddrIter) == fDefaultIPAddr)
        {
            this->SetVal(qtssSvrDefaultDNSName, SocketUtils::GetDNSNameStr(ipAddrIter));
            Assert(this->GetValue(qtssSvrDefaultDNSName)->Ptr != NULL);
            this->SetVal(qtssSvrDefaultIPAddrStr, SocketUtils::GetIPAddrStr(ipAddrIter));
            Assert(this->GetValue(qtssSvrDefaultDNSName)->Ptr != NULL);
            break;
        }
    }
    if (this->GetValue(qtssSvrDefaultDNSName)->Ptr == NULL)
    {
        //If we've gotten here, what has probably happened is the IP address (explicitly
        //entered as a preference) doesn't exist
        QTSSModuleUtils::LogError(qtssFatalVerbosity, qtssMsgDefaultRTSPAddrUnavail, 0);
        return false;   
    }
    return true;
}               


Bool16 QTSServer::CreateListeners(Bool16 startListeningNow, QTSServerPrefs* inPrefs, UInt16 inPortOverride)
{
    struct PortTracking
    {
        PortTracking() : fPort(0), fIPAddr(0), fNeedsCreating(true) {}
        
        UInt16 fPort;
        UInt32 fIPAddr;
        Bool16 fNeedsCreating;
    };
    
    PortTracking* thePortTrackers = NULL;   
    UInt32 theTotalPortTrackers = 0;

    /************************************ 
    *	��ƫ�������ļ���ȡip��ַ 
    ************************************/
    // Get the IP addresses from the pref
    UInt32 theNumAddrs = 0;

	/************************************ 
    *	��ȡIP���� 
    ************************************/
    UInt32* theIPAddrs = this->GetRTSPIPAddrs(inPrefs, &theNumAddrs);   
    UInt32 index = 0;

 //   qtss_printf("[SELF] theNumAddrs %d inPortOverride %d \n",theNumAddrs,inPortOverride);
    if ( inPortOverride != 0)
    {
        theTotalPortTrackers = theNumAddrs; // one port tracking struct for each IP addr
        thePortTrackers = NEW PortTracking[theTotalPortTrackers];
        for (index = 0; index < theNumAddrs; index++)
        {
            thePortTrackers[index].fPort = inPortOverride;
            thePortTrackers[index].fIPAddr = theIPAddrs[index];
        }
    }
    else
    {
        UInt32 theNumPorts = 0;
        /************************************
        *	��ȡ�˿��� 
        ************************************/
        UInt16* thePorts = GetRTSPPorts(inPrefs, &theNumPorts);
        /************************************ 
        *	ip ���� * �˿ڸ��� ����ȡ��ַ��˿ڵ������
        ************************************/
        theTotalPortTrackers = theNumAddrs * theNumPorts;
        thePortTrackers = NEW PortTracking[theTotalPortTrackers];
		//qtss_printf("[SELF] theNumPorts %d  theTotalPortTrackers %u\n",theNumPorts,theTotalPortTrackers);
        UInt32 currentIndex  = 0;

        /************************************
        *	������еĵ�ַ�Ͷ˿ں� 
        ************************************/
        for (index = 0; index < theNumAddrs; index++) /* ����ip */
        {
            for (UInt32 portIndex = 0; portIndex < theNumPorts; portIndex++) /* �����˿� */
            {
                currentIndex = (theNumPorts * index) + portIndex;
                
                thePortTrackers[currentIndex].fPort = thePorts[portIndex];
				//qtss_printf("[SELF] portIndex %d fPort %d \n",portIndex,thePorts[portIndex]);
                thePortTrackers[currentIndex].fIPAddr = theIPAddrs[index];
                char buff[32];
                SocketUtils::UInt32ToStringIp(thePortTrackers[currentIndex].fIPAddr,buff);
 				//qtss_printf("[SELF] index %d theIPAddrs[index] %d fIPAddr %s\n",index,theIPAddrs[index],buff);
            }
        }
                
                delete [] thePorts;
    }
    
        delete [] theIPAddrs;
    //
    // Now figure out which of these ports we are *already* listening on.
    // If we already are listening on that port, just move the pointer to the
    // listener over to the new array
    
    /******************************************************
    *	���ݵ�ַ�Ͷ˿���������� TCPListenerSocket 
    ******************************************************/
    TCPListenerSocket** newListenerArray = NEW TCPListenerSocket*[theTotalPortTrackers];

	/*******************************************************************************************
	*	��ַ�Ͷ˿�������Ƿ����Ѿ����ڵļ�����,������ڣ�ֱ�Ӹ�ֵ�� newListenerArray ������ 
	******************************************************************************************/
    UInt32 curPortIndex = 0;
 	//qtss_printf("[SELF] theTotalPortTrackers %d fNumListeners %d\n",theTotalPortTrackers,fNumListeners);
    for (UInt32 count = 0; count < theTotalPortTrackers; count++)
    {
        for (UInt32 count2 = 0; count2 < fNumListeners; count2++)
        {
            if ((fListeners[count2]->GetLocalPort() == thePortTrackers[count].fPort) &&
                (fListeners[count2]->GetLocalAddr() == thePortTrackers[count].fIPAddr))
            {
                thePortTrackers[count].fNeedsCreating = false;
                newListenerArray[curPortIndex++] = fListeners[count2];
                Assert(curPortIndex <= theTotalPortTrackers);
                break;
            }
        }
    }
    
    /************************************************************************* 
    *	�����µġ�������Ҫ�ļ�����
    *************************************************************************/
    // Create any new listeners we need
    for (UInt32 count3 = 0; count3 < theTotalPortTrackers; count3++)
    {
        if (thePortTrackers[count3].fNeedsCreating)
        {
            newListenerArray[curPortIndex] = NEW RTSPListenerSocket();

            /*******************************************************
            *	��ʼ���µļ����� 
            *******************************************************/
            #if 1
				char ipBuf[32]={'\0'};
				UInt16 port=0;
				SocketUtils::UInt32ToStringIp(thePortTrackers[count3].fIPAddr, ipBuf);
				port=thePortTrackers[count3].fPort;
				qtss_fprintf(stderr,"Listener UInt32Ip %d StrIp %s port %u\n",thePortTrackers[count3].fIPAddr,ipBuf,port);
			#endif
            QTSS_Error err = newListenerArray[curPortIndex]->Initialize(thePortTrackers[count3].fIPAddr, thePortTrackers[count3].fPort);

            char thePortStr[20];
            qtss_sprintf(thePortStr, "%hu", thePortTrackers[count3].fPort);
            
            /*******************************************************
            * ����ʧ�ܣ�ɾ�� 
            *******************************************************/
            // If there was an error creating this listener, destroy it and log an error
            if ((startListeningNow) && (err != QTSS_NoErr)){
                delete newListenerArray[curPortIndex];
			}

            if (err == EADDRINUSE){
                QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssListenPortInUse, 0, thePortStr);
            }else if (err == EACCES){
                QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssListenPortAccessDenied, 0, thePortStr);
            }else if (err != QTSS_NoErr){
                QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssListenPortError, 0, thePortStr);
            }else{
                /*************************************************************************
                * �����ɹ�����ʼ���� 
                *************************************************************************/
                // This listener was successfully created.
                if (startListeningNow){
                    newListenerArray[curPortIndex]->RequestEvent(EV_RE);
				}
                curPortIndex++;
            }
        }
    }
    
    /*******************************************************************************************
    * ɱ�����еĲ�����Ҫ�ļ�����( ɾ���� newListenerArray �����в����ڵļ�����) 
    *******************************************************************************************/
    // Kill any listeners that we no longer need
    for (UInt32 count4 = 0; count4 < fNumListeners; count4++)
    {
        Bool16 deleteThisOne = true;
        
        for (UInt32 count5 = 0; count5 < curPortIndex; count5++)
        {
            if (newListenerArray[count5] == fListeners[count4])
                deleteThisOne = false;
        }
        
        if (deleteThisOne)
            fListeners[count4]->Signal(Task::kKillEvent);
    }
    
    /*************************************************************************
    * �����ɹ�����¼�����ַ����������������������
    *************************************************************************/
    // Finally, make our server attributes and fListener privy to the new...
    fListeners = newListenerArray;
    fNumListeners = curPortIndex;

	#if 1
	qtss_fprintf(stderr,"Listener Numbers %u \n",fNumListeners);
	#endif

	
    UInt32 portIndex = 0;

    /*************************************************************************
    * �������еļ�������������Ǳ��ػػ���ַ����¼�����˿ڵ���������  
    *************************************************************************/
    for (UInt32 count6 = 0; count6 < fNumListeners; count6++)
    {
        if  (fListeners[count6]->GetLocalAddr() != INADDR_LOOPBACK)
        {

            UInt16 thePort = fListeners[count6]->GetLocalPort();

            (void)this->SetValue(qtssSvrRTSPPorts, portIndex, &thePort, sizeof(thePort), QTSSDictionary::kDontObeyReadOnly);
            portIndex++;
        }
    }
    
    /*******************
    * ��¼�˿����� 
    *******************/
    this->SetNumValues(qtssSvrRTSPPorts, portIndex);

    delete [] thePortTrackers;
    return (fNumListeners > 0);
}

UInt32* QTSServer::GetRTSPIPAddrs(QTSServerPrefs* inPrefs, UInt32* outNumAddrsPtr)
{
    UInt32 numAddrs = inPrefs->GetNumValues(qtssPrefsRTSPIPAddr);
    UInt32* theIPAddrArray = NULL;
    
    if (numAddrs == 0)
    {
        *outNumAddrsPtr = 1;
        theIPAddrArray = NEW UInt32[1];
        theIPAddrArray[0] = INADDR_ANY;
    }
    else
    {
        theIPAddrArray = NEW UInt32[numAddrs + 1];
        UInt32 arrIndex = 0;
        
        for (UInt32 theIndex = 0; theIndex < numAddrs; theIndex++)
        {
            // Get the ip addr out of the prefs dictionary
            QTSS_Error theErr = QTSS_NoErr;
            
            char* theIPAddrStr = NULL;
            theErr = inPrefs->GetValueAsString(qtssPrefsRTSPIPAddr, theIndex, &theIPAddrStr);
            if (theErr != QTSS_NoErr)
            {
                delete [] theIPAddrStr;
                break;
            }

            
            UInt32 theIPAddr = 0;
            if (theIPAddrStr != NULL)
            {
                theIPAddr = SocketUtils::ConvertStringToAddr(theIPAddrStr);
                delete [] theIPAddrStr;
                
                if (theIPAddr != 0)
                    theIPAddrArray[arrIndex++] = theIPAddr;
            }   
        }
        
        if ((numAddrs == 1) && (arrIndex == 0))
            theIPAddrArray[arrIndex++] = INADDR_ANY;
        else
            theIPAddrArray[arrIndex++] = INADDR_LOOPBACK;
    
        *outNumAddrsPtr = arrIndex;
    }
    
    return theIPAddrArray;
}

UInt16* QTSServer::GetRTSPPorts(QTSServerPrefs* inPrefs, UInt32* outNumPortsPtr)
{
    *outNumPortsPtr = inPrefs->GetNumValues(qtssPrefsRTSPPorts);
    
    if (*outNumPortsPtr == 0)
        return NULL;
        
    UInt16* thePortArray = NEW UInt16[*outNumPortsPtr];
    
    for (UInt32 theIndex = 0; theIndex < *outNumPortsPtr; theIndex++)
    {
        // Get the ip addr out of the prefs dictionary
        UInt32 theLen = sizeof(UInt16);
        QTSS_Error theErr = QTSS_NoErr;
        theErr = inPrefs->GetValue(qtssPrefsRTSPPorts, theIndex, &thePortArray[theIndex], &theLen);
        Assert(theErr == QTSS_NoErr);   
    }
    
    return thePortArray;
}

Bool16  QTSServer::SetupUDPSockets()
{
	/*
		���� rtp/rtcp ����ʲô��;
	*/
    //function finds all IP addresses on this machine, and binds 1 RTP / RTCP
    //socket pair to a port pair on each address.

    UInt32 theNumAllocatedPairs = 0;
    for (UInt32 theNumPairs = 0; theNumPairs < SocketUtils::GetNumIPAddrs(); theNumPairs++)
    {
        UDPSocketPair* thePair = fSocketPool->CreateUDPSocketPair(SocketUtils::GetIPAddr(theNumPairs), 0);
                if (thePair != NULL)
                {
            theNumAllocatedPairs++;
                        thePair->GetSocketA()->RequestEvent(EV_RE);
                        thePair->GetSocketB()->RequestEvent(EV_RE);
                }
        }
    //only return an error if we couldn't allocate ANY pairs of sockets
    if (theNumAllocatedPairs == 0)
        {
                fServerState = qtssFatalErrorState; // also set the state to fatal error
                return false;
        }
    return true;
}

Bool16  QTSServer::SwitchPersonality()
{
#ifndef __Win32__  //not supported
    OSCharArrayDeleter runGroupName(fSrvrPrefs->GetRunGroupName());
    OSCharArrayDeleter runUserName(fSrvrPrefs->GetRunUserName());

    if (::strlen(runGroupName.GetObject()) > 0)
    {
        struct group* gr = ::getgrnam(runGroupName.GetObject());
        if (gr == NULL || ::setgid(gr->gr_gid) == -1)
        {
            char buffer[kErrorStrSize];

            QTSSModuleUtils::LogError(qtssFatalVerbosity, qtssMsgCannotSetRunGroup, 0,
                    runGroupName.GetObject(), qtss_strerror(OSThread::GetErrno(), buffer, sizeof(buffer)));
            return false;
        }
    }
    
    if (::strlen(runUserName.GetObject()) > 0)
    {
        struct passwd* pw = ::getpwnam(runUserName.GetObject());
        if (pw == NULL || ::setuid(pw->pw_uid) == -1)
        {
            QTSSModuleUtils::LogError(qtssFatalVerbosity, qtssMsgCannotSetRunUser, 0,
                    runUserName.GetObject(), strerror(OSThread::GetErrno()));
            return false;
        }
    }

#endif  
   return true;
}




void    QTSServer::LoadCompiledInModules()
{
#ifndef DSS_DYNAMIC_MODULES_ONLY
    // MODULE DEVELOPERS SHOULD ADD THE FOLLOWING THREE LINES OF CODE TO THIS
    // FUNCTION IF THEIR MODULE IS BEING COMPILED INTO THE SERVER.
    //
    // QTSSModule* myModule = new QTSSModule("__MODULE_NAME__");
    // (void)myModule->Initialize(&sCallbacks, &__MODULE_MAIN_ROUTINE__);
    // (void)AddModule(myModule);
    //
    // The following modules are all compiled into the server. 

	/************************************************************************
	*
	* �ļ�ģ�飬���������RTSP����
	* ��ȡ��ý���ļ������͵��ͻ���
	*
	*************************************************************************/
    QTSSModule* theFileModule = new QTSSModule("QTSSFileModule");
    (void)theFileModule->SetupModule(&sCallbacks, &QTSSFileModule_Main);
    (void)AddModule(theFileModule);
	///qtss_printf("QTSSFileModule start succeed \n");

	/************************************************************************
	*
	* ����ģ��
	*
	*************************************************************************/
    QTSSModule* theReflectorModule = new QTSSModule("QTSSReflectorModule");
    (void)theReflectorModule->SetupModule(&sCallbacks, &QTSSReflectorModule_Main);
    (void)AddModule(theReflectorModule);
	///qtss_printf("QTSSReflectorModule start succeed \n");

	/************************************************************************
	*
	* �м�ģ��
	*
	*************************************************************************/
	QTSSModule* theRelayModule = new QTSSModule("QTSSRelayModule");
    (void)theRelayModule->SetupModule(&sCallbacks, &QTSSRelayModule_Main);
    (void)AddModule(theRelayModule);
	///qtss_printf("QTSSRelayModule start succeed \n");

	/************************************************************************
	*
	*  ��־��ȡģ��
	*
	*************************************************************************/
    QTSSModule* theAccessLog = new QTSSModule("QTSSAccessLogModule");
    (void)theAccessLog->SetupModule(&sCallbacks, &QTSSAccessLogModule_Main);
    (void)AddModule(theAccessLog);
    ///qtss_printf("QTSSAccessLogModule start succeed \n");

	/************************************************************************
	*
	* ����ģ��
	*
	*************************************************************************/
    QTSSModule* theFlowControl = new QTSSModule("QTSSFlowControlModule");
    (void)theFlowControl->SetupModule(&sCallbacks, &QTSSFlowControlModule_Main);
    (void)AddModule(theFlowControl);
	///qtss_printf("QTSSFlowControlModule start succeed \n");

	/************************************************************************
	*
	* posix �ļ�ϵͳģ��
	*
	*************************************************************************/
    QTSSModule* theFileSysModule = new QTSSModule("QTSSPosixFileSysModule");
    (void)theFileSysModule->SetupModule(&sCallbacks, &QTSSPosixFileSysModule_Main);
    (void)AddModule(theFileSysModule);
    ///qtss_printf("QTSSPosixFileSysModule start succeed \n");

	/************************************************************************
	*
	* ����Աģ��(ֻ����HTTP����)
	*
	*************************************************************************/
    QTSSModule* theAdminModule = new QTSSModule("QTSSAdminModule");
    (void)theAdminModule->SetupModule(&sCallbacks, &QTSSAdminModule_Main);
    (void)AddModule(theAdminModule);
  	///qtss_printf("QTSSAdminModule start succeed \n");

	/************************************************************************
	*
	* mp3��ģ��(ֻ����HTTP)
	*
	*************************************************************************/
    QTSSModule* theMP3StreamingModule = new QTSSModule("QTSSMP3StreamingModule");
    (void)theMP3StreamingModule->SetupModule(&sCallbacks, &QTSSMP3StreamingModule_Main);
    (void)AddModule(theMP3StreamingModule);
  	///qtss_printf("QTSSMP3StreamingModule start succeed \n");

	/************************************************************************
	*
	* �洢ģ��
	*
	*************************************************************************/
    QTSSModule* theQTACESSmodule = new QTSSModule("QTSSAccessModule");
    (void)theQTACESSmodule->SetupModule(&sCallbacks, &QTSSAccessModule_Main);
    (void)AddModule(theQTACESSmodule);
    ///qtss_printf("QTSSAccessModule start succeed \n");

#endif //DSS_DYNAMIC_MODULES_ONLY

	/************************************************************************
	*
	* ����ģ��
	*
	*************************************************************************/
#ifdef PROXYSERVER
    QTSSModule* theProxyModule = new QTSSModule("QTSSProxyModule");
    (void)theProxyModule->SetupModule(&sCallbacks, &QTSSProxyModule_Main);
    (void)AddModule(theProxyModule);
	///qtss_printf("QTSSProxyModule start succeed \n");
#endif


}



void    QTSServer::InitCallbacks()
{
    sCallbacks.addr[kNewCallback] =                 (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_New;
    sCallbacks.addr[kDeleteCallback] =              (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Delete;
    sCallbacks.addr[kMillisecondsCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Milliseconds;
    sCallbacks.addr[kConvertToUnixTimeCallback] =   (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_ConvertToUnixTime;

    sCallbacks.addr[kAddRoleCallback] =             (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_AddRole;
    sCallbacks.addr[kCreateObjectTypeCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_CreateObjectType;
    sCallbacks.addr[kAddAttributeCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_AddAttribute;
    sCallbacks.addr[kIDForTagCallback] =            (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_IDForAttr;
    sCallbacks.addr[kGetAttributePtrByIDCallback] = (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_GetValuePtr;
    sCallbacks.addr[kGetAttributeByIDCallback] =    (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_GetValue;
    sCallbacks.addr[kSetAttributeByIDCallback] =    (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_SetValue;
    sCallbacks.addr[kCreateObjectValueCallback] =   (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_CreateObject;
    sCallbacks.addr[kGetNumValuesCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_GetNumValues;

    sCallbacks.addr[kWriteCallback] =               (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Write;
    sCallbacks.addr[kWriteVCallback] =              (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_WriteV;
    sCallbacks.addr[kFlushCallback] =               (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Flush;
    sCallbacks.addr[kReadCallback] =                (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Read;
    sCallbacks.addr[kSeekCallback] =                (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Seek;
    sCallbacks.addr[kAdviseCallback] =              (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Advise;

    sCallbacks.addr[kAddServiceCallback] =          (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_AddService;
    sCallbacks.addr[kIDForServiceCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_IDForService;
    sCallbacks.addr[kDoServiceCallback] =           (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_DoService;

    sCallbacks.addr[kSendRTSPHeadersCallback] =     (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_SendRTSPHeaders;
    sCallbacks.addr[kAppendRTSPHeadersCallback] =   (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_AppendRTSPHeader;
    sCallbacks.addr[kSendStandardRTSPCallback] =    (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_SendStandardRTSPResponse;

    sCallbacks.addr[kAddRTPStreamCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_AddRTPStream;
    sCallbacks.addr[kPlayCallback] =                (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Play;
    sCallbacks.addr[kPauseCallback] =               (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Pause;
    sCallbacks.addr[kTeardownCallback] =            (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Teardown;
    sCallbacks.addr[kRefreshTimeOutCallback] =      (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_RefreshTimeOut;

    sCallbacks.addr[kRequestEventCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_RequestEvent;
    sCallbacks.addr[kSetIdleTimerCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_SetIdleTimer;
    sCallbacks.addr[kSignalStreamCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_SignalStream;

    sCallbacks.addr[kOpenFileObjectCallback] =      (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_OpenFileObject;
    sCallbacks.addr[kCloseFileObjectCallback] =     (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_CloseFileObject;

    sCallbacks.addr[kCreateSocketStreamCallback] =  (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_CreateStreamFromSocket;
    sCallbacks.addr[kDestroySocketStreamCallback] = (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_DestroySocketStream;

    sCallbacks.addr[kAddStaticAttributeCallback] =          (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_AddStaticAttribute;
    sCallbacks.addr[kAddInstanceAttributeCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_AddInstanceAttribute;
    sCallbacks.addr[kRemoveInstanceAttributeCallback] =     (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_RemoveInstanceAttribute;

    sCallbacks.addr[kGetAttrInfoByIndexCallback] =          (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_GetAttrInfoByIndex;
    sCallbacks.addr[kGetAttrInfoByNameCallback] =           (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_GetAttrInfoByName;
    sCallbacks.addr[kGetAttrInfoByIDCallback] =             (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_GetAttrInfoByID;
    sCallbacks.addr[kGetNumAttributesCallback] =            (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_GetNumAttributes;


    sCallbacks.addr[kGetValueAsStringCallback] =            (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_GetValueAsString;
    sCallbacks.addr[kTypeToTypeStringCallback] =            (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_TypeToTypeString;
    sCallbacks.addr[kTypeStringToTypeCallback] =            (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_TypeStringToType;
    sCallbacks.addr[kStringToValueCallback] =               (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_StringToValue;
    sCallbacks.addr[kValueToStringCallback] =               (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_ValueToString;

    sCallbacks.addr[kRemoveValueCallback] =                 (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_RemoveValue;

    sCallbacks.addr[kRequestGlobalLockCallback] =           (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_RequestLockedCallback;
    sCallbacks.addr[kIsGlobalLockedCallback] =              (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_IsGlobalLocked;
    sCallbacks.addr[kUnlockGlobalLock] =                    (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_UnlockGlobalLock;

    sCallbacks.addr[kAuthenticateCallback] =                (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Authenticate;
    sCallbacks.addr[kAuthorizeCallback] =                   (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_Authorize;
    
    sCallbacks.addr[kLockObjectCallback] =                  (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_LockObject;
    sCallbacks.addr[kUnlockObjectCallback] =                (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_UnlockObject;
    sCallbacks.addr[kSetAttributePtrCallback] =             (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_SetValuePtr;
    
    sCallbacks.addr[kSetIntervalRoleTimerCallback] =        (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_SetIdleRoleTimer;
    
    sCallbacks.addr[kLockStdLibCallback] =                  (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_LockStdLib;
    sCallbacks.addr[kUnlockStdLibCallback] =                (QTSS_CallbackProcPtr)QTSSCallbacks::QTSS_UnlockStdLib;
}

void QTSServer::LoadModules(QTSServerPrefs* inPrefs)
{
    // Fetch the name of the module directory and open it.
    OSCharArrayDeleter theModDirName(inPrefs->GetModuleDirectory());
    
#ifdef __Win32__
    // NT doesn't seem to have support for the POSIX directory parsing APIs.
    OSCharArrayDeleter theLargeModDirName(NEW char[::strlen(theModDirName.GetObject()) + 3]);
    ::strcpy(theLargeModDirName.GetObject(), theModDirName.GetObject());
    ::strcat(theLargeModDirName.GetObject(), "\\*");
    
    WIN32_FIND_DATA theFindData;
    HANDLE theSearchHandle = ::FindFirstFile(theLargeModDirName.GetObject(), &theFindData);
    
    if (theSearchHandle == INVALID_HANDLE_VALUE)
    {
        QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssMsgNoModuleFolder, 0);  
        return;
    }
    
    while (theSearchHandle != INVALID_HANDLE_VALUE)
    {
        this->CreateModule(theModDirName.GetObject(), theFindData.cFileName);
        
        if (!::FindNextFile(theSearchHandle, &theFindData))
        {
            ::FindClose(theSearchHandle);
            theSearchHandle = INVALID_HANDLE_VALUE;
        }
    }
#else       

    // POSIX version
	// opendir mallocs memory for DIR* so call closedir to free the allocated memory
    DIR* theDir = ::opendir(theModDirName.GetObject());
    if (theDir == NULL)
    {
        QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssMsgNoModuleFolder, 0);  
        return;
    }
    
    while (true)
    {
        // Iterate over each file in the directory, attempting to construct
        // a module object from that file.
        
        struct dirent* theFile = ::readdir(theDir);
        if (theFile == NULL)
            break;
        
        this->CreateModule(theModDirName.GetObject(), theFile->d_name);
    }
	
	(void)::closedir(theDir);
	
#endif
}

void    QTSServer::CreateModule(char* inModuleFolderPath, char* inModuleName)
{
    // Ignore these silly directory names

	/*
		���ģ�����Ƿ�Ϸ�
	*/
    if (::strlen(inModuleName) == 0 
				|| inModuleName==NULL)
        return;
    /* 
    	����Ƿ��ǵ�ǰĿ¼ 
    */   
    if (::strcmp(inModuleName, ".") == 0)
        return;
	/*
		����Ƿ��Ǹ�Ŀ¼
	*/
	if (::strcmp(inModuleName, "..") == 0)
        return;
	
    if (*inModuleName == '.')
        return; // Fix 2572248. Do not attempt to load '.' files as modules at all 

    /* Ϊ��ģ�鹹��һ��ȫ·�� */
    // Construct a full path to this module
    UInt32 totPathLen = ::strlen(inModuleFolderPath) + ::strlen(inModuleName);
    OSCharArrayDeleter theModPath(NEW char[totPathLen + 4]);
    ::strcpy(theModPath.GetObject(), inModuleFolderPath);
    ::strcat(theModPath.GetObject(), kPathDelimiterString);
    ::strcat(theModPath.GetObject(), inModuleName);
            
    /* 
    	����ģ����� 
   	*/
    // Construct a QTSSModule object, and attempt to initialize the module
    QTSSModule* theNewModule = NEW QTSSModule(inModuleName, theModPath.GetObject());
	/*
		����ģ�����
	*/
	QTSS_Error theErr = theNewModule->SetupModule(&sCallbacks);
    /* 
		���ó���
	*/
    if (theErr != QTSS_NoErr)
    {
        QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssMsgBadModule, theErr,
                                        inModuleName);
        delete theNewModule;
    }
    /* 
		���óɹ� ��ʼ�������ӵ�����
    */
    // If the module was successfully initialized, add it to our module queue
    else if (!this->AddModule(theNewModule))
    {
        QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssMsgRegFailed, theErr,
                                        inModuleName);
        delete theNewModule;
    }
}

Bool16 QTSServer::AddModule(QTSSModule* inModule)
{
    Assert(inModule->IsInitialized());
    // Prepare to invoke the module's Register role. Setup the Register param block
    QTSS_ModuleState theModuleState;

	/*********************************************************** 
	*	
	* �����߳�˽������(��˽�����ݻ���ģ��ע��ʱ����) 
	*
	**********************************************************/
    theModuleState.curModule = inModule;
    theModuleState.curRole = QTSS_Register_Role;
    theModuleState.curTask = NULL;
    OSThread::SetMainThreadData(&theModuleState);
    
    // Currently we do nothing with the module name
    QTSS_RoleParams theRegParams;
    theRegParams.regParams.outModuleName[0] = 0;
	
    /**********************************************************
    *	
    * ����ģ�� QTSS_Register_Role ��ɫ��ע��ģ��֧�ֵĽ�ɫ
    *
   	**********************************************************/
    // If the module returns an error from the QTSS_Register role, don't put it anywhere
    if (inModule->CallDispatch(QTSS_Register_Role, &theRegParams) != QTSS_NoErr)
        return false;

	/********************************************************** 
    *	 
    * ����߳�˽������
    *
    **********************************************************/
    OSThread::SetMainThreadData(NULL);

	/**********************************************************
	*	����ģ����
	**********************************************************/
    //
    // Update the module name to reflect what was returned from the register role
    theRegParams.regParams.outModuleName[QTSS_MAX_MODULE_NAME_LENGTH - 1] = 0;
    if (theRegParams.regParams.outModuleName[0] != 0)
        inModule->SetValue(qtssModName, 0, theRegParams.regParams.outModuleName, ::strlen(theRegParams.regParams.outModuleName), false);

    /********************************************************** 
	*	Ϊ��ģ������һ��ƫ�������ֵ�
    **********************************************************/
    // Give the module object a prefs dictionary. Instance attributes are allowed for these objects.
    QTSSPrefs* thePrefs = NEW QTSSPrefs( sPrefsSource, inModule->GetValue(qtssModName), QTSSDictionaryMap::GetMap(QTSSDictionaryMap::kModulePrefsDictIndex), true);
    thePrefs->RereadPreferences();
    inModule->SetPrefsDict(thePrefs);
     
    /**********************************************************
	*	����ģ�鵽ģ������(Server �ֵ��ڵ�����)
	**********************************************************/
    // Add this module to the array of module (dictionaries)
    UInt32 theNumModules = this->GetNumValues(qtssSvrModuleObjects);
    QTSS_Error theErr = this->SetValue(qtssSvrModuleObjects, theNumModules, &inModule, sizeof(QTSSModule*), QTSSDictionary::kDontObeyReadOnly);
    Assert(theErr == QTSS_NoErr);
    
    /**********************************************************
	*	����ģ�鵽ģ�����
	**********************************************************/
    // Add this module to the module queue
    sModuleQueue.EnQueue(inModule->GetQueueElem());

    return true;
}

void QTSServer::BuildModuleRoleArrays()
{
    OSQueueIter theIter(&sModuleQueue);
    QTSSModule* theModule = NULL;
    /********************************************************************************************************************
		��ɫ-ģ�鲼��
		+------------------------------------------------
		| ��ɫ1 | ��ɫ2 | ��ɫ3 | ��ɫ4 | ��ɫ5|...
		+------------------------------------------------
           |       |       |       |
           |	   |       |       | 
           |	   |       |       +------------------------------------------------
      	   |	   |       |       | ģ��1 | ģ��2 | ģ��3 | ...
      	   |	   |       |       +------------------------------------------------
           |	   |       |
           |	   |       +------------------------------------------------
      	   |	   |       | ģ��1 | ģ��2 | ģ��3 | ...
      	   |	   |       +------------------------------------------------
           |	   |
           |       +------------------------------------------------
      	   |	   | ģ��1 | ģ��2 | ģ��3 | ...
      	   |	   +------------------------------------------------
           |
      	   +------------------------------------------------
      	   | ģ��1 | ģ��2 | ģ��3 | ...
      	   +------------------------------------------------
	***************************************************************************************************************/   
    // Make sure these variables are cleaned out in case they've already been inited.
    /************************************************************
	*	������ǰ�����Ľ�ɫ����
	*********************************************************/
    DestroyModuleRoleArrays();

    // Loop through all the roles of all the modules, recording the number of
    // modules in each role, and also recording which modules are doing what.

	/*********************************************************
	*	�������н�ɫ
	*********************************************************/
    for (UInt32 x = 0; x < QTSSModule::kNumRoles; x++)
    {
        sNumModulesInRole[x] = 0;
		/*********************************************************
		*	����ģ����У�����ע���˵�ǰ��ɫ��ģ�飬�����м���
		*********************************************************/
        for (theIter.Reset(); !theIter.IsDone(); theIter.Next())
        {
            theModule = (QTSSModule*)theIter.GetCurrent()->GetEnclosingObject();
            if (theModule->RunsInRole(x))
                sNumModulesInRole[x] += 1;
        }
    	/*********************************************************
		*	��ǰ��ɫ��ĳһģ��ע��
		*********************************************************/
        if (sNumModulesInRole[x] > 0)
        {
            UInt32 moduleIndex = 0;
			/*********************************************************
			*	Ϊ��ǰ��ɫ����ģ������
			*********************************************************/
            sModuleArray[x] = new QTSSModule*[sNumModulesInRole[x] + 1];

			/*****************************************************************************************************************
			*	��������ģ����У�����ģ��ָ�븳ֵ����ǰ��ɫ��ģ������
			*****************************************************************************************************************/
            for (theIter.Reset(); !theIter.IsDone(); theIter.Next())
            {
                theModule = (QTSSModule*)theIter.GetCurrent()->GetEnclosingObject();
                if (theModule->RunsInRole(x))
                {
                    sModuleArray[x][moduleIndex] = theModule;
                    moduleIndex++;
                }
            }
        }
    }
}

void QTSServer::DestroyModuleRoleArrays()
{
    for (UInt32 x = 0; x < QTSSModule::kNumRoles; x++)
    {
        sNumModulesInRole[x] = 0;
        if (sModuleArray[x] != NULL)
            delete [] sModuleArray[x];
        sModuleArray[x] = NULL; 
    }
}
 
void QTSServer::DoInitRole()
{
    QTSS_RoleParams theInitParams;
    theInitParams.initParams.inServer =         this;
    theInitParams.initParams.inPrefs =          fSrvrPrefs;
    theInitParams.initParams.inMessages =       fSrvrMessages;
    theInitParams.initParams.inErrorLogStream = &sErrorLogStream;
        
    QTSS_ModuleState theModuleState;
    theModuleState.curRole = QTSS_Initialize_Role;
    theModuleState.curTask = NULL;
    OSThread::SetMainThreadData(&theModuleState);

    //
    // Add the OPTIONS method as the one method the server handles by default (it handles
    // it internally). Modules that handle other RTSP methods will add 
    QTSS_RTSPMethod theOptionsMethod = qtssOptionsMethod;
    (void)this->SetValue(qtssSvrHandledMethods, 0, &theOptionsMethod, sizeof(theOptionsMethod));


// For now just disable the SetParameter to be compatible with Real.  It should really be removed only for clients that have problems with their SetParameter implementations like (Real Players).
// At the moment it isn't necesary to add the option.
//   QTSS_RTSPMethod	theSetParameterMethod = qtssSetParameterMethod;
//    (void)this->SetValue(qtssSvrHandledMethods, 0, &theSetParameterMethod, sizeof(theSetParameterMethod));
	
    for (UInt32 x = 0; x < QTSServerInterface::GetNumModulesInRole(QTSSModule::kInitializeRole); x++)
    {
        QTSSModule* theModule = QTSServerInterface::GetModule(QTSSModule::kInitializeRole, x);
        theInitParams.initParams.inModule = theModule;
        theModuleState.curModule = theModule;
        QTSS_Error theErr = theModule->CallDispatch(QTSS_Initialize_Role, &theInitParams);

        if (theErr != QTSS_NoErr)
        {
            // If the module reports an error when initializing itself,
            // delete the module and pretend it was never there.
            QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssMsgInitFailed, theErr,
                                            theModule->GetValue(qtssModName)->Ptr);
                                            
            sModuleQueue.Remove(theModule->GetQueueElem());
            delete theModule;
        }
    }
    this->SetupPublicHeader();

    OSThread::SetMainThreadData(NULL);
}

void QTSServer::SetupPublicHeader()
{
    //
    // After the Init role, all the modules have reported the methods that they handle.
    // So, we can prune this attribute for duplicates, and construct a string to use in the
    // Public: header of the OPTIONS response
    QTSS_RTSPMethod* theMethod = NULL;
    UInt32 theLen = 0;

    Bool16 theUniqueMethods[qtssNumMethods + 1];
    ::memset(theUniqueMethods, 0, sizeof(theUniqueMethods));

    for (UInt32 y = 0; this->GetValuePtr(qtssSvrHandledMethods, y, (void**)&theMethod, &theLen) == QTSS_NoErr; y++)
        theUniqueMethods[*theMethod] = true;

    // Rewrite the qtssSvrHandledMethods, eliminating any duplicates that modules may have introduced
    UInt32 uniqueMethodCount = 0;
    for (QTSS_RTSPMethod z = 0; z < qtssNumMethods; z++)
    {
        if (theUniqueMethods[z])
            this->SetValue(qtssSvrHandledMethods, uniqueMethodCount++, &z, sizeof(QTSS_RTSPMethod));
    }
    this->SetNumValues(qtssSvrHandledMethods, uniqueMethodCount);
    
    // Format a text string for the Public: header
    ResizeableStringFormatter theFormatter(NULL, 0);

    for (UInt32 a = 0; this->GetValuePtr(qtssSvrHandledMethods, a, (void**)&theMethod, &theLen) == QTSS_NoErr; a++)
    {
        sPublicHeaderFormatter.Put(RTSPProtocol::GetMethodString(*theMethod));
        sPublicHeaderFormatter.Put(", ");
    }
    sPublicHeaderStr.Ptr = sPublicHeaderFormatter.GetBufPtr();
    sPublicHeaderStr.Len = sPublicHeaderFormatter.GetBytesWritten() - 2; //trunc the last ", "
}


Task*   RTSPListenerSocket::GetSessionTask(TCPSocket** outSocket)
{
    Assert(outSocket != NULL);
    
    // when the server is behing a round robin DNS, the client needs to knwo the IP address ot the server
    // so that it can direct the "POST" half of the connection to the same machine when tunnelling RTSP thru HTTP
    Bool16  doReportHTTPConnectionAddress = QTSServerInterface::GetServer()->GetPrefs()->GetDoReportHTTPConnectionAddress();
    
    RTSPSession* theTask = NEW RTSPSession(doReportHTTPConnectionAddress);
    *outSocket = theTask->GetSocket();  // out socket is not attached to a unix socket yet.

    if (this->OverMaxConnections(0))
        this->SlowDown();
    else
        this->RunNormal();
        
    return theTask;
}


Bool16 RTSPListenerSocket::OverMaxConnections(UInt32 buffer)
{
    QTSServerInterface* theServer = QTSServerInterface::GetServer();
    SInt32 maxConns = theServer->GetPrefs()->GetMaxConnections();
    Bool16 overLimit = false;
    
    if (maxConns > -1) // limit connections
    { 
        maxConns += buffer;
        if  ( (theServer->GetNumRTPSessions() > (UInt32) maxConns) 
              ||
              ( theServer->GetNumRTSPSessions() + theServer->GetNumRTSPHTTPSessions() > (UInt32) maxConns ) 
            )
        {
            overLimit = true;          
        }
    } 
    return overLimit;
     
}


UDPSocketPair*  RTPSocketPool::ConstructUDPSocketPair()
{
    Task* theTask = ((QTSServer*)QTSServerInterface::GetServer())->fRTCPTask;
    
    //construct a pair of UDP sockets, the lower one for RTP data (outgoing only, no demuxer
    //necessary), and one for RTCP data (incoming, so definitely need a demuxer).
    //These are nonblocking sockets that DON'T receive events (we are going to poll for data)
	// They do receive events - we don't poll from them anymore
    return NEW
        UDPSocketPair(  NEW UDPSocket(theTask, Socket::kNonBlockingSocketType),
                        NEW UDPSocket(theTask, UDPSocket::kWantsDemuxer | Socket::kNonBlockingSocketType));
}

void RTPSocketPool::DestructUDPSocketPair(UDPSocketPair* inPair)
{
    delete inPair->GetSocketA();
    delete inPair->GetSocketB();
    delete inPair;
}

void RTPSocketPool::SetUDPSocketOptions(UDPSocketPair* inPair)
{
    // Apparently the socket buffer size matters even though this is UDP and being
    // used for sending... on UNIX typically the socket buffer size doesn't matter because the
    // packet goes right down to the driver. On Win32 and linux, unless this is really big, we get packet loss.
    inPair->GetSocketA()->SetSocketBufSize(256 * 1024);

    //
    // Always set the Rcv buf size for the RTCP sockets. This is important because the
    // server is going to be getting many many acks.
    UInt32 theRcvBufSize = QTSServerInterface::GetServer()->GetPrefs()->GetRTCPSocketRcvBufSizeinK();

    //
    // In case the rcv buf size is too big for the system, retry, dividing the requested size by 2.
    // Until it works, or until some minimum value is reached.
    OS_Error theErr = EINVAL;
    while ((theErr != OS_NoErr) && (theRcvBufSize > 32))
    {
        theErr = inPair->GetSocketB()->SetSocketRcvBufSize(theRcvBufSize * 1024);
        if (theErr != OS_NoErr)
            theRcvBufSize >>= 1;
    }

    //
    // Report an error if we couldn't set the socket buffer size the user requested
    if (theRcvBufSize != QTSServerInterface::GetServer()->GetPrefs()->GetRTCPSocketRcvBufSizeinK())
    {
        char theRcvBufSizeStr[20];
        qtss_sprintf(theRcvBufSizeStr, "%lu", theRcvBufSize);
        //
        // For now, do not log an error, though we should enable this in the future.
        //QTSSModuleUtils::LogError(qtssWarningVerbosity, qtssMsgSockBufSizesTooLarge, theRcvBufSizeStr);
    }
}



QTSS_Error QTSServer::RereadPrefsService(QTSS_ServiceFunctionArgsPtr /*inArgs*/)
{
    //
    // This function can only be called safely when the server is completely running.
    // Ensuring this is a bit complicated because of preemption. Here's how it's done...
    
    QTSServerInterface* theServer = QTSServerInterface::GetServer();
    
    // This is to make sure this function isn't being called before the server is
    // completely started up.
    if ((theServer == NULL) || (theServer->GetServerState() != qtssRunningState))
        return QTSS_OutOfState;

    // Because the server must have started up, and because this object always stays
    // around (until the process dies), we can now safely get this object.
    QTSServerPrefs* thePrefs = theServer->GetPrefs();
    
    // Grab the prefs mutex. We want to make sure that calls to RereadPrefsService
    // are serialized. This also prevents the server from shutting down while in
    // this function, because the QTSServer destructor grabs this mutex as well.
    OSMutexLocker locker(thePrefs->GetMutex());
    
    // Finally, check the server state again. The state may have changed
    // to qtssShuttingDownState or qtssFatalErrorState in this time, though
    // at this point we have the prefs mutex, so we are guarenteed that the
    // server can't actually shut down anymore
    if (theServer->GetServerState() != qtssRunningState)
        return QTSS_OutOfState;
    
    // Ok, we're ready to reread preferences now.
    
    //
    // Reread preferences
    sPrefsSource->Parse();
    thePrefs->RereadServerPreferences(true);
    
    {
        //
        // Update listeners, ports, and IP addrs.
        OSMutexLocker locker(theServer->GetServerObjectMutex());
        (void)((QTSServer*)theServer)->SetDefaultIPAddr();
		#if 1
		qtss_fprintf(stderr,"%s %d \n",__FUNCTION__,__LINE__);
		#endif
        (void)((QTSServer*)theServer)->CreateListeners(true, thePrefs, 0);
    }
    
    // Delete all the streams
    QTSSModule** theModule = NULL;
    UInt32 theLen = 0;
    
    for (int y = 0; QTSServerInterface::GetServer()->GetValuePtr(qtssSvrModuleObjects, y, (void**)&theModule, &theLen) == QTSS_NoErr; y++)
    {
        Assert(theModule != NULL);
        Assert(theLen == sizeof(QTSSModule*));
        #if 1
			qtss_fprintf(stderr," [%s] [%d] y=%d \n",__FUNCTION__,__LINE__,y);
		#endif
        (*theModule)->GetPrefsDict()->RereadPreferences();

#if DEBUG
        theModule = NULL;
        theLen = 0;
#endif
    }
    
    //
    // Go through each module's prefs object and have those reread as well

    //
    // Now that we are done rereading the prefs, invoke all modules in the RereadPrefs
    // role so they can update their internal prefs caches.
    for (UInt32 x = 0; x < QTSServerInterface::GetNumModulesInRole(QTSSModule::kRereadPrefsRole); x++)
    {
        QTSSModule* theModule = QTSServerInterface::GetModule(QTSSModule::kRereadPrefsRole, x);
		#if 1
			qtss_fprintf(stderr," [%s] [%d] x=%d \n",__FUNCTION__,__LINE__,x);
		#endif
        (void)theModule->CallDispatch(QTSS_RereadPrefs_Role, NULL);
    }
    return QTSS_NoErr;
}

