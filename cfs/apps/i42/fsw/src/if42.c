/* 
** Purpose: 42 Interface
**
** Notes:
**   1. Use 42's ReadFromSocket() and WriteToSocket() because they contain
**      autogenerated interface code that uses AcStruct. Don't use 42's iokit's
**      InitSocketClient() becuase it exits the program on errors.
**
** References:
**   1. OpenSat Object-based Application Developer's Guide
**   2. cFS Application Developer's Guide
**   3. 42/Docs/Standalone AcApp text file
**
** License:
**   Written by David McComas, licensed under the copyleft GNU
**   General Public License (GPL). 
**
*/

/*
** Include Files:
*/

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#include "if42.h"

/* 42 AC struct access macros */
#define AC42          &(If42->Ac42->Obj)  
#define AC42_(field)  (If42->Ac42->Obj.field)  



/*
** 42 Function Prototypes not in a header 
*/

extern int  ReadFromSocket(SOCKET Socket, struct AcType *AC);
extern void WriteToSocket(SOCKET Socket, struct AcType *AC);

/*
** Global File Data
*/

static IF42_Class* If42 = NULL;



/*
** Local Function Prototypes
*/

static boolean InitClientSocket(const char *HostName, uint16 Port, boolean AllowBlocking);
static void SocketTask(void);

/******************************************************************************
** Function: IF42_Constructor
**
** Initialize the 42 interface.
**
** Notes:
**   1. This must be called prior to any other function.
**   2. AC structure initialization mimics 42's AcApp.c dynamic memory 
**      configuration so AcApp's functions can be used unmodified.
**
*/
void IF42_Constructor(IF42_Class*  If42Obj, const char* IpAddrStr, uint16 Port) {

   int32 CfeStatus;

   If42 = If42Obj;

   CFE_PSP_MemSet((void*)If42, 0, sizeof(IF42_Class));
   
   If42->Connected = FALSE;
   If42->Port = Port;
   strncpy(If42->IpAddrStr, IpAddrStr, IF42_IP_ADDR_STR_LEN);

   CFE_SB_InitMsg(&If42->SensorPkt, I42_SENSOR_DATA_MID, IF42_SENSOR_DATA_PKT_LEN, TRUE);
   
   /* Create semaphore (given by parent to wake-up child) */
   CfeStatus = OS_BinSemCreate(&If42->WakeUpSemaphore, I42_CHILD_SEM_NAME, OS_SEM_EMPTY, 0);
   
   if (CfeStatus != CFE_SUCCESS) {
      
      CFE_EVS_SendEvent(IF42_CREATE_SEM_ERR_EID, CFE_EVS_ERROR,
                        "Failed to create %s semaphore. Status=0x%8X", I42_CHILD_SEM_NAME, (int)CfeStatus);
   }

   AC42_Constructor();
   
} /* End IF42_Constructor() */


/******************************************************************************
** Function: IF42_Close
**
*/
void IF42_Close(void) {

   if (If42->Connected == TRUE) {
    
      close(If42->SocketFd);
   
      If42->Connected = FALSE;
      OS_BinSemGive(If42->WakeUpSemaphore); /* Allow child to terminate gracefully */
      
      CFE_EVS_SendEvent(IF42_SOCKET_CLOSE_EID, CFE_EVS_INFORMATION,
                        "Successfully closed socket");

  
   } /* End if connected */
   else {
      CFE_EVS_SendEvent(IF42_SOCKET_CLOSE_EID, CFE_EVS_DEBUG,
                        "Attempt to close socket without a connection");
   }

   If42->ActuatorPktSent = FALSE;
   IF42_ResetStatus();

} /* End IF42_Close() */


/******************************************************************************
** Function: IF42_ConnectCmd
**
*/
boolean IF42_ConnectCmd(void* ObjDataPtr, const CFE_SB_MsgPtr_t MsgPtr)
{

   const IF42_ConnectCmdMsg* CmdMsg = (const IF42_ConnectCmdMsg *) MsgPtr;
   uint32 AppId;
   int32  CfeStatus;
   CFE_ES_AppInfo_t AppInfo;
   boolean FailedToGetAppInfo = TRUE;
   
   IF42_Close();

   strncpy(If42->IpAddrStr, CmdMsg->IpAddrStr, IF42_IP_ADDR_STR_LEN);
   If42->Port = CmdMsg->Port;
   
   /* InitSocket reports errors */
   If42->Connected = InitClientSocket(If42->IpAddrStr, If42->Port, TRUE);    /* FALSE = Non-blocking */

   if (If42->Connected) {
      
      CFE_EVS_SendEvent(IF42_CONNECT_TO_42_EID, CFE_EVS_INFORMATION,
                        "Connected to 42 simulator on %s port %d", If42->IpAddrStr,If42->Port);

      If42->InitCycle = TRUE;
      
      CfeStatus = CFE_ES_GetAppIDByName(&AppId, I42_APP_NAME);
      if (CfeStatus == CFE_SUCCESS) { 
         CfeStatus = CFE_ES_GetAppInfo(&AppInfo, AppId);
         if (CfeStatus == CFE_SUCCESS) {
            FailedToGetAppInfo = FALSE;
            if (AppInfo.NumOfChildTasks == 0) {
         
               CfeStatus = CFE_ES_CreateChildTask(&If42->ChildTaskId, I42_CHILD_NAME,
                                                  SocketTask, 0, I42_CHILD_STACK_SIZE,
                                                  I42_CHILD_PRIORITY, 0);      
               if (CfeStatus != CFE_SUCCESS) {
                  
                  CFE_EVS_SendEvent(IF42_CREATE_CHILD_ERR_EID, CFE_EVS_ERROR,
                                    "Failed to create child task %s. Status=0x%8X", I42_CHILD_NAME, (int)CfeStatus);               
                  IF42_Close();
               }
            }
         }
      }
      if (FailedToGetAppInfo) {
         
         CFE_EVS_SendEvent(IF42_CONNECT_TO_42_EID, CFE_EVS_INFORMATION,
                           "App info check for socket child task failed so unknown state. cFE return status=0x%8X", (int)CfeStatus);               
         
      }
      
      OS_BinSemGive(If42->WakeUpSemaphore);
      
   } /* End if connected */

   return If42->Connected;

} /* End IF42_Connect42Cmd() */


/******************************************************************************
** Function: IF42_DisconnectCmd
**
** Notes:
**   1. Must match CMDMGR_CmdFuncPtr function signature
*/
boolean IF42_DisconnectCmd(void* ObjDataPtr, const CFE_SB_MsgPtr_t MsgPtr)
{

   IF42_Close();
   
   return TRUE;
   
} /* End IF42_DisconnectCmd() */


/******************************************************************************
** Function:  IF42_ResetStatus
**
** Only counters are reset and boolean state information remains intact. If
** changes are made be sure to check all of the calling scenarios to make sure
** any assumptions aren't violated.
*/
void IF42_ResetStatus(void) {

   If42->ExecuteCycleCnt  = 0;
   If42->SensorPktCnt     = 0;
   If42->ActuatorPktCnt   = 0;
   If42->UnclosedCycleCnt = 0;
  
} /* End IF42_ResetStatus() */


/******************************************************************************
** Function:  IF42_ManageExecution
**
** Notes:
**   1. Giving the semaphore signals the child task to receive sensor data
**   2. An 'unclosed cycle' is when the sensor-controller-actuator cycle didn't
**      complete before this function was called again. If this persists past
**      a limit then the logic will force a new cycle to begin by giving the
**      the child task the semaphore 
**   3. The WakeUpSemaphore can become invalid when the child task is
**      intentionally terminated in a disconnect scenario so don't take any
**      action. 
*/
void IF42_ManageExecution(void)
{

   CFE_EVS_SendEvent(IF42_DEBUG_EID, CFE_EVS_DEBUG, 
                     "*** IF42_App::ManageExecution(%d): WakeUpSemaphore=%08X, ActuatorPktSent=%d",
                     If42->ExecuteCycleCnt, If42->WakeUpSemaphore, If42->ActuatorPktSent);
   
   if (If42->Connected) {
    
      if (If42->InitCycle) {
               
         CFE_EVS_SendEvent(IF42_SKIP_INIT_CYCLE_EID, CFE_EVS_INFORMATION,
                           "Skipping scheduler execution request during init cycle");
      
         return;
      }
      
      if (If42->WakeUpSemaphore != IF42_SEM_INVALID) { 
         
         if (If42->ActuatorPktSent == TRUE) {
         
            CFE_EVS_SendEvent(IF42_DEBUG_EID, CFE_EVS_DEBUG,
                              "**** IF42_ManageExecution(): Giving semaphore - WakeUpSemaphore=%08X, ActuatorPktSent=%d",
                              If42->WakeUpSemaphore,If42->ActuatorPktSent);
            
            OS_BinSemGive(If42->WakeUpSemaphore);
            If42->UnclosedCycleCnt = 0;
         
         }
         else {
            
            ++If42->UnclosedCycleCnt;
            if (If42->UnclosedCycleCnt > I42_EXECUTE_UNCLOSED_CYCLE_LIM) {
         
               /* Consider restarting child task if this doesn't fix the issue. If the issue occurs! */
               CFE_EVS_SendEvent(IF42_NO_ACTUATOR_CMD_EID, CFE_EVS_ERROR,
                                 "Actuator command not received for %d execution cycles. Giving child semaphore",
                                 If42->UnclosedCycleCnt);

               OS_BinSemGive(If42->WakeUpSemaphore); 
               If42->UnclosedCycleCnt = 0;
            
            }/* End if unclosed cycle */
         } /* End if no actuator packet */
      } /* End if semaphore valid */
       
   } /* End if connected */
   else {
  
      IF42_ResetStatus();
   
   }

} /* IF42_ManageExecution() */


/******************************************************************************
** Function: IF42_RecvSensorData
**
*/
boolean IF42_RecvSensorData(IF42_SensorDataPkt* SensorDataPkt)
{

   int i;
   
   
   CFE_EVS_SendEvent(IF42_DEBUG_EID, CFE_EVS_DEBUG,
                     "**** IF42_RecvSensorData(): ExeCnt=%d, SnrCnt=%d, ActCnt=%d, ActSent=%d\n",
                     If42->ExecuteCycleCnt, If42->SensorPktCnt, If42->ActuatorPktCnt, If42->ActuatorPktSent);

   If42->Ac42 = AC42_GetPtr();

   AC42_(EchoEnabled) = FALSE;
   ReadFromSocket(If42->SocketFd,AC42);

   GyroProcessing(AC42);
   MagnetometerProcessing(AC42);
   CssProcessing(AC42);
   FssProcessing(AC42);
   StarTrackerProcessing(AC42);
   GpsProcessing(AC42);
   
   SensorDataPkt->Time = AC42_(Time); 
   SensorDataPkt->GpsValid  = TRUE;
   SensorDataPkt->StValid   = TRUE;
   SensorDataPkt->SunValid  = AC42_(SunValid);
   SensorDataPkt->InitCycle = If42->InitCycle;

   for (i=0; i < 3; i++) {
   
      SensorDataPkt->PosN[i] = AC42_(PosN[i]);  /* GPS */
      SensorDataPkt->VelN[i] = AC42_(VelN[i]);
   
      SensorDataPkt->qbn[i]  = AC42_(qbn[i]);   /* ST */

      SensorDataPkt->wbn[i]  = AC42_(wbn[i]);   /* Gyro */
   
      SensorDataPkt->svb[i]  = AC42_(svb[i]);   /* CSS/FSS */

      SensorDataPkt->bvb[i]  = AC42_(bvb[i]);   /* MTB */
   
      SensorDataPkt->WhlH[i] = AC42_(Whl[i].H); /* Wheels */
   }

   SensorDataPkt->qbn[3]  = AC42_(qbn[3]);
   SensorDataPkt->WhlH[3] = AC42_(Whl[3].H);
   
   AC42_ReleasePtr(If42->Ac42);

   return TRUE;

} /* IF42_RecvSensorData() */


/******************************************************************************
** Function: IF42_SendActuatorCmds
**
*/
boolean IF42_SendActuatorCmds(const IF42_ActuatorCmdDataPkt* ActuatorCmdDataPkt) 
{

   int i;

   CFE_EVS_SendEvent(IF42_DEBUG_EID, CFE_EVS_DEBUG,
                     "**** IF42_SendActuatorCmds(): ExeCnt=%d, SnrCnt=%d, ActCnt=%d, ActSent=%d\n",
                     If42->ExecuteCycleCnt, If42->SensorPktCnt, If42->ActuatorPktCnt, If42->ActuatorPktSent);
   
   If42->Ac42 = AC42_GetPtr();

   for (i=0; i < 3; i++) {
  
      AC42_(Tcmd[i]) = ActuatorCmdDataPkt->Tcmd[i];
      AC42_(Mcmd[i]) = ActuatorCmdDataPkt->Mcmd[i];
 
   }
   
   AC42_(G[0].Cmd.Ang[0]) = ActuatorCmdDataPkt->SaGcmd;

   WheelProcessing(AC42);
   MtbProcessing(AC42);
   
   WriteToSocket(If42->SocketFd,AC42);
   
   AC42_ReleasePtr(If42->Ac42);
   
   If42->InitCycle = FALSE;
   ++If42->ActuatorPktCnt;
   If42->ActuatorPktSent = TRUE;
      
   return TRUE;

} /* End IF42_SendActuatorCmds() */


/******************************************************************************
** Function:  InitClientSocket
**
** Ported from 42's ioktit.c InitSocketClient(). 
*/
static boolean InitClientSocket(const char *HostName, uint16 Port, boolean AllowBlocking)
{

   int    Flags;
   int    DisableNagle = 1;
   struct sockaddr_in  Server;
   struct hostent*     Host;

   If42->Connected = FALSE;
   strcpy(If42->IpAddrStr, HostName);  

   If42->SocketFd = socket(AF_INET,SOCK_STREAM,0);
   if (If42->SocketFd < 0) {
      CFE_EVS_SendEvent(IF42_SOCKET_OPEN_ERR_EID,CFE_EVS_ERROR,
                        "Error opening socket. Socket() return code = %d", If42->SocketFd);
      return(If42->Connected);
   }

   Host = gethostbyname(HostName);
   if (Host == NULL) {
     CFE_EVS_SendEvent(IF42_HOST_ERR_EID,CFE_EVS_ERROR, "Server %s not found", HostName);
     return(If42->Connected);
   }

   memset((char *) &Server,0,sizeof(Server));
   Server.sin_family = AF_INET;
   memcpy((char *)&Server.sin_addr.s_addr,(char *)Host->h_addr_list[0], Host->h_length);
   Server.sin_port = htons(Port);
   
   CFE_EVS_SendEvent(IF42_DEBUG_EID, CFE_EVS_DEBUG, "*** IF42 ***: Attempting to connect to Server %s on Port %d\n",HostName, Port);
   if (connect(If42->SocketFd,(struct sockaddr *) &Server, sizeof(Server)) < 0) {
      CFE_EVS_SendEvent(IF42_CONNECT_ERR_EID,CFE_EVS_ERROR,
                        "Error connecting client socket: %s", strerror(errno));
     return(If42->Connected);
   }
   else {
      If42->Connected = TRUE;
      CFE_EVS_SendEvent(IF42_DEBUG_EID, CFE_EVS_INFORMATION, "Successfully connected to Server %s on Port %d\n",HostName, Port);
   }

   /* Keep read() from waiting for message to come */
   if (!AllowBlocking) {
      Flags = fcntl(If42->SocketFd, F_GETFL, 0);
      fcntl(If42->SocketFd,F_SETFL, Flags|O_NONBLOCK);
   }

   /* Allow TCP to send small packets (look up Nagle's algorithm) */
   /* Depending on your message sizes, this may or may not improve performance */
   setsockopt(If42->SocketFd,IPPROTO_TCP,TCP_NODELAY,&DisableNagle,sizeof(DisableNagle));
   
   return If42->Connected;
      
} /* End InitClientSocket() */


/******************************************************************************
** Function: SocketTask
**
** Notes:
**   1. This infinite loop design proved to be the most robust. I tried to 
**      create/terminate the child task with a socket connect/disconnect and
**      something didn't seem to get cleaned up properly and the system would
**      hang on a second connect cmd. 
*/
static void SocketTask(void)
{

   int32 CfeStatus;
   
   CfeStatus = CFE_ES_RegisterChildTask();

   If42->ActuatorPktSent = TRUE; 
   
   if (CfeStatus == CFE_SUCCESS) {
   
      CFE_EVS_SendEvent(IF42_CHILD_TASK_INIT_EID, CFE_EVS_INFORMATION, "IF42 child task initialization complete");

      while (TRUE) {
         
         CFE_EVS_SendEvent(IF42_DEBUG_EID, CFE_EVS_DEBUG,
                           "\n\n**** SocketTask(%d) Waiting for semaphore: InitCycle=%d\n",
                           If42->ExecuteCycleCnt, If42->InitCycle);    
         
         CfeStatus = OS_BinSemTake(If42->WakeUpSemaphore); /* Pend until parent app gives semaphore */

         /* Check connection for termination scenario */
         if ((CfeStatus == CFE_SUCCESS)&& (If42->Connected)) {
            
            ++If42->ExecuteCycleCnt;
            if (IF42_RecvSensorData(&(If42->SensorPkt)) > 0) {

               CFE_SB_TimeStampMsg((CFE_SB_Msg_t *) &(If42->SensorPkt));
               CfeStatus = CFE_SB_SendMsg((CFE_SB_Msg_t *) &(If42->SensorPkt));
               
               if (CfeStatus == CFE_SUCCESS) {
                  ++If42->SensorPktCnt;
                  If42->ActuatorPktSent = FALSE;
               }
            }
            else {
               
               If42->Connected = FALSE;
            
            }
         
         } /* End if valid semaphore */
             
      } /* End task while loop */
      
   } /* End if CFE_ES_RegisterChildTask() successful */
   else {
       
      CFE_EVS_SendEvent(IF42_CHILD_TASK_ERR_EID, CFE_EVS_ERROR, 
                        "IF42 call to CFE_ES_RegisterChildTask() failed, Status=%d",
                        (int)CfeStatus);
   }

   If42->WakeUpSemaphore = IF42_SEM_INVALID;  /* Prevent parent from invoking the child task */
   
   CFE_ES_ExitChildTask();  /* Clean-up system resources */

} /* End SocketTask() */
