////////////////////////////////////////////////////////////////////// 
// VirtualBox VMs - managed by an NT Service (VBoxVmService)
//////////////////////////////////////////////////////////////////////

#include <atlbase.h>
#include <stdio.h>
#include <windows.h>
#include <winbase.h>
#include <winsvc.h>
#include <process.h>
#include <strsafe.h>
#include <time.h>
#include <stdarg.h>
#include <io.h>
#include <winerror.h>
#include "VirtualBox.h"

#include "VBoxVmPipeManager.h"


const int nBufferSize = 500;
char pServiceName[nBufferSize+1];
char pExeFile[nBufferSize+1];
char pInitFile[nBufferSize+1];
char pLogFile[nBufferSize+1];
const int nMaxProcCount = 100;
int nProcStatus[nMaxProcCount];
IVirtualBox *virtualBox;
ISession *session = NULL;

SERVICE_STATUS          serviceStatus; 
SERVICE_STATUS_HANDLE   hServiceStatusHandle; 

VOID WINAPI VBoxVmServiceMain( DWORD dwArgc, LPTSTR *lpszArgv );
VOID WINAPI VBoxVmServiceHandler( DWORD fdwControl );
char *pipelcat(LPPIPEINST pipe, const char *src, const int srclength);

#define SAFE_RELEASE(x) \
    if (x) { \
        x->Release(); \
        x = NULL; \
    }

void WriteLog(char* pMsg)
{
    // write error or other information into log file
    try
    {
        SYSTEMTIME oT;
        ::GetLocalTime(&oT);
        FILE* pLog = fopen(pLogFile,"a");
        fprintf(pLog,"%02d/%02d/%04d, %02d:%02d:%02d - %s\n",oT.wMonth,oT.wDay,oT.wYear,oT.wHour,oT.wMinute,oT.wSecond,pMsg); 
        fclose(pLog);
    } catch(...) {}
}

// Write to both the pipe and log
void WriteLogPipe(LPPIPEINST pipe, LPCSTR formatstring, ...){

    char pTemp[nBufferSize+1];
    DWORD pLen;
    va_list args;
    va_start(args, formatstring);

    pLen = _vsnprintf(pTemp, sizeof(pTemp), formatstring, args);

    //append to log
    WriteLog(pTemp);

    //append to pipe
    if (pipe)
        pipelcat(pipe, pTemp, pLen);
}

// Routine to make human readable description, instead of just the error id.
char *ErrorString(DWORD err)
{
    const DWORD buffsize = 300+1;
    static char buff[buffsize];

    if(FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,
                NULL,
                err,
                0,
                buff,
                buffsize,
                NULL) == 0)
    { 
        // FormatMessage failed
        sprintf_s(buff,"Unknown error with error code = %d", err);
    }
    return buff;
} // ErrorString

////////////////////////////////////////////////////////////////////// 
//
// Configuration Data and Tables
//

SERVICE_TABLE_ENTRY   DispatchTable[] = 
{ 
    {pServiceName, VBoxVmServiceMain}, 
    {NULL, NULL}
};


// helper functions
// My own implementation of strlcat, but with optional length param. Used for appending to the pipe.
char *pipelcat(LPPIPEINST pipe, const char *src, int srclength) {


    if (pipe==NULL) {return NULL;}

    if (srclength == -1) {
        srclength = (int)strlen(src);
    }
    //Debug: uncoment to see write stat
    //printf("pipelcat(cbToWrite=%d, srclength=%d)\n",pipe->cbToWrite,srclength);

    // test for free space, If we don't have space for the whole string we just return.
    if (((srclength + pipe->cbToWrite)) > (sizeof(pipe->chReply) -1)) {
        return NULL;
    }

    strncpy(pipe->chReply + pipe->cbToWrite, src, srclength);
    pipe->cbToWrite += srclength;

    pipe->chReply[pipe->cbToWrite +1] = '\0'; //Amen 

    //Debug: Uncoment to se new size in cbToWrite
    //printf("~pipelcat(cbToWrite=%d)\n",pipe->cbToWrite);
    return pipe->chReply;
}

char *nIndexTopVmName(int nIndex) {
    char pItem[nBufferSize+1];
    static char pVmName[nBufferSize+1];

    sprintf_s(pItem,"Vm%d\0",nIndex);

    // get VmName
    GetPrivateProfileString(pItem,"VmName","",pVmName,nBufferSize,pInitFile);
    if (strlen(pVmName) == 0) {
        return NULL;
    }

    return pVmName;
}

// Get VM status with given name
void VMStatus(char *vm, LPPIPEINST pipe)
{
    CComBSTR vmName(vm);
    HRESULT rc;
    IMachine *machine = NULL;

    rc = virtualBox->FindMachine(vmName, &machine);

    if (FAILED(rc))
    {
        IErrorInfo *errorInfo;

        rc = GetErrorInfo(0, &errorInfo);

        if (FAILED(rc))
            WriteLogPipe(pipe,"Error getting error info! rc = 0x%x\n", rc); 
        else
        {
            BSTR errorDescription = NULL;

            rc = errorInfo->GetDescription(&errorDescription);

            if (FAILED(rc) || !errorDescription)
                WriteLogPipe(pipe, "Error getting error description! rc = 0x%x\n", rc);
            else
            {
                WriteLogPipe(pipe, "Error finding machine: %S\n", errorDescription);
                SysFreeString(errorDescription);
            }

            SAFE_RELEASE(errorInfo);
        }
    }
    else
    {
        MachineState state;

        rc = machine->get_State(&state);
        if (!SUCCEEDED(rc))
        {
            WriteLogPipe(pipe, "Error retrieving machine state! rc = 0x%x\n", rc);
        }
        else
        {
            switch (state) {
                case MachineState_PoweredOff:
                    WriteLogPipe(pipe, "VM %s is powered off.\n", vm);
                    break;
                case MachineState_Saved:
                    WriteLogPipe(pipe, "VM %s is saved.\n", vm);
                    break;
                case MachineState_Aborted:
                    WriteLogPipe(pipe, "VM %s is aborted.\n", vm);
                    break;
                case MachineState_Running:
                    WriteLogPipe(pipe, "VM %s is running.\n", vm);
                    break;
                case MachineState_Paused:
                    WriteLogPipe(pipe, "VM %s is paused.\n", vm);
                    break;
                default:
                    WriteLogPipe(pipe, "VM %s is in unknown state %d.\n", vm, state);
                    break;
            }
        }

        SAFE_RELEASE(machine);
    }

}

// start a VM with given name
int StartVM(char *vm, LPPIPEINST pipe)
{
    CComBSTR vmName(vm);
    HRESULT rc;
    IMachine *machine = NULL;
    int result = 1;

    rc = virtualBox->FindMachine(vmName, &machine);

    if (FAILED(rc))
    {
        IErrorInfo *errorInfo;

        rc = GetErrorInfo(0, &errorInfo);

        if (FAILED(rc))
            WriteLogPipe(pipe,"Error getting error info! rc = 0x%x\n", rc); 
        else
        {
            BSTR errorDescription = NULL;

            rc = errorInfo->GetDescription(&errorDescription);

            if (FAILED(rc) || !errorDescription)
                WriteLogPipe(pipe, "Error getting error description! rc = 0x%x\n", rc);
            else
            {
                WriteLogPipe(pipe, "Error finding machine: %S\n", errorDescription);
                SysFreeString(errorDescription);
            }

            SAFE_RELEASE(errorInfo);
        }
    }
    else
    {
        BSTR sessiontype = SysAllocString(L"headless");
        IProgress *progress = NULL;
        BSTR guid;

        do
        {
            rc = machine->get_Id(&guid); // Get the GUID of the machine.
            if (!SUCCEEDED(rc))
            {
                WriteLogPipe(pipe, "Error retrieving machine ID! rc = 0x%x\n", rc);
                break;
            }

            // Start a VM session using the delivered VBox GUI.
            rc = machine->LaunchVMProcess(session, sessiontype,
                    NULL, &progress);
            if (!SUCCEEDED(rc))
            {
                WriteLogPipe(pipe, "Could not open remote session! rc = 0x%x\n", rc);
                break;
            }
            result = 0;

            // Wait until VM is running. 
            WriteLogPipe(pipe, "Starting VM %s, please wait ... ", vm);
            rc = progress->WaitForCompletion (-1);
            WriteLogPipe(pipe, "done.\n", vm);

            // Close the session.
            rc = session->UnlockMachine();

        } while (0);

        SAFE_RELEASE(progress);
        SysFreeString(guid);
        SysFreeString(sessiontype);
        SAFE_RELEASE(machine);
    }
    return result;
}


// start a VM with given index
BOOL StartProcess(int nIndex, LPPIPEINST pipe) 
{ 
    char *cp;

    cp = nIndexTopVmName( nIndex );
    if (cp == NULL) 
    {
        if (pipe)  // this message is invalid when StartProcess is called during service start, not by control command
            WriteLogPipe(pipe, "Unknown VM index number. Are you sure it is defined in the VBoxVmService.ini file?"); 
        return false; 
    }

    // if called during service start, check if AutoStart is disabled
    if (pipe == NULL)
    {
        char pAutoStart[nBufferSize+1];
        char pItem[nBufferSize+1];

        // get AutoStart
        sprintf_s(pItem,"Vm%d\0",nIndex);
        GetPrivateProfileString(pItem,"AutoStart","",pAutoStart,nBufferSize,pInitFile);
        if (stricmp(pAutoStart, "no") == 0)
            return true;
    }

    if (StartVM(cp, pipe) == 0)
        nProcStatus[nIndex] = 1;
    return true;

}

// stop a VM with given name
int StopVM(char *vm, char *method, LPPIPEINST pipe)
{
    CComBSTR vmName(vm);
    HRESULT rc;
    IMachine *machine = NULL;
    int result = 1;

    rc = virtualBox->FindMachine(vmName, &machine);

    if (FAILED(rc))
    {
        IErrorInfo *errorInfo;

        rc = GetErrorInfo(0, &errorInfo);

        if (FAILED(rc))
            WriteLogPipe(pipe,"Error getting error info! rc = 0x%x\n", rc); 
        else
        {
            BSTR errorDescription = NULL;

            rc = errorInfo->GetDescription(&errorDescription);

            if (FAILED(rc) || !errorDescription)
                WriteLogPipe(pipe, "Error getting error description! rc = 0x%x\n", rc);
            else
            {
                WriteLogPipe(pipe, "Error finding machine: %S\n", errorDescription);
                SysFreeString(errorDescription);
            }

            SAFE_RELEASE(errorInfo);
        }
    }
    else
    {
        IConsole *console = NULL;
        IProgress *progress = NULL;
        BSTR guid;

        do
        {
            rc = machine->get_Id(&guid); // Get the GUID of the machine.
            if (!SUCCEEDED(rc))
            {
                WriteLogPipe(pipe, "Error retrieving machine ID! rc = 0x%x\n", rc);
                break;
            }

            // Start a VM session using the delivered VBox GUI.
            rc = machine->LockMachine(session, LockType_Shared);
            if (!SUCCEEDED(rc))
            {
                WriteLogPipe(pipe, "Could not lock machine! rc = 0x%x\n", rc);
                break;
            }

            // Get console object. 
            session->get_Console(&console);

            if (strcmp(method, "savestate") == 0)
                rc = console->SaveState(&progress);
            else
                rc = console->PowerButton();
            if (!SUCCEEDED(rc))
            {
                WriteLogPipe(pipe, "Could not stop machine! rc = 0x%x\n", rc);
                break;
            }

            result = 0;

            WriteLogPipe(pipe, "VM %s is being shutdown.", vm);

            // Close the session.
            rc = session->UnlockMachine();

        } while (0);

        SAFE_RELEASE(console);
        SAFE_RELEASE(progress);
        SysFreeString(guid);
        SAFE_RELEASE(machine);
    }
    return result;
}

// end a VM with given index
BOOL EndProcess(int nIndex, LPPIPEINST pipe) 
{
    char pItem[nBufferSize+1];
    char pShutdownMethod[nBufferSize+1];
    char *cp;


    cp = nIndexTopVmName( nIndex );
    if (cp==NULL) {WriteLogPipe(pipe, "Unknown VM index number. Are you sure it is defined in the VBoxVmService.ini file?"); return false; }

    // get ShutdownMethod
    sprintf_s(pItem,"Vm%d\0",nIndex);
    GetPrivateProfileString(pItem,"ShutdownMethod","",pShutdownMethod,nBufferSize,pInitFile);

    StopVM(cp, pShutdownMethod, pipe);
    nProcStatus[nIndex] = 0;
    return true;
}

////////////////////////////////////////////////////////////////////// 
//
// This routine gets used to start your service
//
VOID WINAPI VBoxVmServiceMain( DWORD dwArgc, LPTSTR *lpszArgv )
{
    DWORD   status = 0; 
    DWORD   specificError = 0xfffffff; 

    serviceStatus.dwServiceType        = SERVICE_WIN32; 
    serviceStatus.dwCurrentState       = SERVICE_START_PENDING; 
    serviceStatus.dwControlsAccepted   = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PAUSE_CONTINUE; 
    serviceStatus.dwWin32ExitCode      = 0; 
    serviceStatus.dwServiceSpecificExitCode = 0; 
    serviceStatus.dwCheckPoint         = 0; 
    serviceStatus.dwWaitHint           = 0; 

    hServiceStatusHandle = RegisterServiceCtrlHandler(pServiceName, VBoxVmServiceHandler); 
    if (hServiceStatusHandle==0) 
    {
        long nError = GetLastError();
        char pTemp[121];
        sprintf_s(pTemp, "RegisterServiceCtrlHandler failed, error code = %d", nError);
        WriteLog(pTemp);
        return; 
    } 

    // Initialization complete - report running status 
    serviceStatus.dwCurrentState       = SERVICE_RUNNING; 
    serviceStatus.dwCheckPoint         = 0; 
    serviceStatus.dwWaitHint           = 0;  
    if(!SetServiceStatus(hServiceStatusHandle, &serviceStatus)) 
    { 
        long nError = GetLastError();
        char pTemp[121];
        sprintf_s(pTemp, "SetServiceStatus failed, error code = %d", nError);
        WriteLog(pTemp);
    } 

}

////////////////////////////////////////////////////////////////////// 
//
// This routine responds to events concerning your service, like start/stop
//
VOID WINAPI VBoxVmServiceHandler(DWORD fdwControl)
{
    switch(fdwControl) 
    {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:

            //tell PipeManager to exit.
            PipeManagerExit();
            // also tell SCM we'll need some time to shutdown
            serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            {
                char pPause[nBufferSize+1];
                GetPrivateProfileString("Settings","PauseShutdown","1000",pPause,nBufferSize,pInitFile);
                serviceStatus.dwWaitHint = atoi(pPause) + 1000;  
            }
            break;
        case SERVICE_CONTROL_PAUSE:
            serviceStatus.dwCurrentState = SERVICE_PAUSED; 
            break;
        case SERVICE_CONTROL_CONTINUE:
            serviceStatus.dwCurrentState = SERVICE_RUNNING; 
            break;
        case SERVICE_CONTROL_INTERROGATE:
            break;
        default: 
            {
                long nError = GetLastError();
                char pTemp[121];
                sprintf_s(pTemp,  "Unrecognized opcode %d", fdwControl);
                WriteLog(pTemp);
            }
    };
    if (!SetServiceStatus(hServiceStatusHandle,  &serviceStatus)) 
    { 
        long nError = GetLastError();
        char pTemp[121];
        sprintf_s(pTemp, "SetServiceStatus failed, error code = %d", nError);
        WriteLog(pTemp);
    } 
}


void WorkerHandleCommand(LPPIPEINST pipe) 
{
    char pTemp[121];
    char buffer[80]; 
    char *cp;
    memset(buffer, 0, 80);


    StringCchCopy(buffer, nBufferSize, pipe->chRequest);

    sprintf_s(pTemp, "Received control command: %s", buffer);
    WriteLog(pTemp);

    // process command
    if (strncmp(buffer, "start", 5) == 0)
    {
        int nIndex = atoi(buffer + 6);
        StartProcess(nIndex,pipe);
    }
    else if (strncmp(buffer, "stop", 4) == 0)
    {
        int nIndex = atoi(buffer + 5);
        EndProcess(nIndex,pipe);
    }
    else if (strncmp(buffer, "status", 6) == 0)
    {
        cp = nIndexTopVmName( atoi(buffer + 7) );
        if (cp==NULL)
        {
            WriteLogPipe(pipe, "Unknown VM index number. Are you sure it is defined in the VBoxVmService.ini file?"); 
            return;
        }
        VMStatus(cp, pipe);
    }
    else if (strncmp(buffer, "shutdown", 8) == 0)
    {
        VBoxVmServiceHandler(SERVICE_CONTROL_SHUTDOWN);
    }
    else {
        printf("Unknown command \"%s\"\n", buffer);
    }
}

void listVMs(IVirtualBox *virtualBox)
{
    HRESULT rc;

    SAFEARRAY *machinesArray = NULL;
    WriteLogPipe(NULL, "List all the VMs found by VBoxVmService"); 
 
    rc = virtualBox->get_Machines(&machinesArray);
    if (SUCCEEDED(rc))
    {
        IMachine **machines;
        rc = SafeArrayAccessData (machinesArray, (void **) &machines);
        if (SUCCEEDED(rc))
        {
            for (ULONG i = 0; i < machinesArray->rgsabound[0].cElements; ++i)
            {
                BSTR str;

                rc = machines[i]->get_Name(&str);
                if (SUCCEEDED(rc))
                {
                    WriteLogPipe(NULL, "Name: %S", str); 
                    SysFreeString(str);
                }
            }

            SafeArrayUnaccessData (machinesArray);
        }

        SafeArrayDestroy (machinesArray);
    }
}

unsigned __stdcall WorkerProc(void* pParam)
{
    HRESULT rc;
    rc = CoInitialize(NULL);

    rc = CoCreateInstance(CLSID_VirtualBox,
            NULL,
            CLSCTX_LOCAL_SERVER,
            IID_IVirtualBox,
            (void**)&virtualBox);
    if (!SUCCEEDED(rc))
    {
        WriteLogPipe(NULL, "Error creating VirtualBox instance! rc = 0x%x\n", rc);
        return 1;
    }

    // Create the session object.
    rc = CoCreateInstance(CLSID_Session, // the VirtualBox base object
            NULL,          // no aggregation
            CLSCTX_INPROC_SERVER, // the object lives in a server process on this machine
            IID_ISession,  // IID of the interface
            (void**)&session);
    if (!SUCCEEDED(rc))
    {
        WriteLogPipe(NULL, "Error creating Session instance! rc = 0x%x\n", rc);
        return 1;
    }

    listVMs(virtualBox);

    // Startup VMs
    for(int i=0;i<nMaxProcCount;i++)
        nProcStatus[i] = 0;

    BOOL bShouldWait = FALSE;
    for(int i=0;i<nMaxProcCount;i++)
    {
        if (!StartProcess(i, NULL))
            break;
        bShouldWait = true;
    }

    // wait for VMs to start up
    if (bShouldWait)
    {
        char pPause[nBufferSize+1];
        GetPrivateProfileString("Settings","PauseStartup","1000",pPause,nBufferSize,pInitFile);
        Sleep(atoi(pPause));
    }
    const int nMessageSize = 80;

    //PipeManager will handel all pipe connections
    PipeManager("\\\\.\\pipe\\VBoxVmService", WorkerHandleCommand);

    //gracefully ending the service
    bShouldWait = FALSE;
    // terminate all running processes before shutdown
    for(int i=nMaxProcCount-1;i>=0;i--)
    {
        if (nProcStatus[i] == 1)
        {
            EndProcess(i, NULL);
            bShouldWait = TRUE;
        }
    }           

    // wait for VMs to shut down
    if (bShouldWait)
    {
        char pPause[nBufferSize+1];
        GetPrivateProfileString("Settings","PauseShutdown","1000",pPause,nBufferSize,pInitFile);
        Sleep(atoi(pPause));
    }

    // notify SCM that we've finished
    serviceStatus.dwCurrentState = SERVICE_STOPPED; 
    serviceStatus.dwWaitHint     = 0;  
    if (!SetServiceStatus(hServiceStatusHandle, &serviceStatus))
    { 
        long nError = GetLastError();
        char pTemp[121];
        sprintf_s(pTemp, "SetServiceStatus failed, error code = %d", nError);
        WriteLog(pTemp);
    }

    SAFE_RELEASE(session);
    virtualBox->Release();
    CoUninitialize();

    return 1;
}

////////////////////////////////////////////////////////////////////// 
//
// Standard C Main
//
void main(int argc, char *argv[] )
{
    // initialize variables for .exe, .ini, and .log file names
    char pModuleFile[nBufferSize+1];
    DWORD dwSize = GetModuleFileName(NULL,pModuleFile,nBufferSize);
    pModuleFile[dwSize] = 0;
    HANDLE hThread;
    unsigned threadID;

    if(dwSize>4&&pModuleFile[dwSize-4]=='.')
    {
        sprintf_s(pExeFile,"%s",pModuleFile);
        *(strrchr(pModuleFile, '\\')) = 0;
        sprintf_s(pInitFile,"%s\\VBoxVmService.ini",pModuleFile);
        sprintf_s(pLogFile,"%s\\VBoxVmService.log",pModuleFile);
    }
    else
    {
        printf("Invalid module file name: %s\r\n", pModuleFile);
        return;
    }
    WriteLog(pExeFile);
    WriteLog(pInitFile);
    WriteLog(pLogFile);
    // read service name from .ini file
    GetPrivateProfileString("Settings","ServiceName","VBoxVmService",pServiceName,nBufferSize,pInitFile);
    WriteLog(pServiceName);

    // Run in debug mode if switch is "-d"
    if(argc==2&&_stricmp("-d",argv[1])==0)
    {
        printf("Welcome to vms (debug mode)\n");
        WorkerProc( NULL );
    }
    else 
    {   

        hThread = (HANDLE)_beginthreadex( NULL, 0, &WorkerProc, NULL, 0, &threadID );
        // start a worker thread to check for pipe messages
        if(hThread==0)
        {
            char pTemp[121];
            sprintf_s(pTemp, "_beginthread failed, error code = %d", GetLastError());
            WriteLog(pTemp);
        }


        // pass dispatch table to service controller
        if(!StartServiceCtrlDispatcher(DispatchTable))
        {
            char pTemp[121];
            sprintf_s(pTemp, "StartServiceCtrlDispatcher failed, error code = %d", GetLastError());
            WriteLog(pTemp);
        }
        // you don't get here unless the service is shutdown

        // Destroy the thread object.
        CloseHandle( hThread );

    }
}
