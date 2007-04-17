/*----------------------------------------------------------------------**
*  file:        drvModbusTCPAsyn.c                                      **
*-----------------------------------------------------------------------**
* EPICS asyn driver support for Modbus protocol communication with PLCs **
* over TCP/IP.                                                          **
* 
* Mark Rivers, University of Chicago
* Original Date March 3, 2007
*
* Based on the modtcp and plctcp code from Rolf Keitel of Triumf, with  **
* work from Ivan So at NSLS.
*-----------------------------------------------------------------------**
*
*/


/* ANSI C includes  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* EPICS includes */
#include <dbAccess.h>
#include <epicsStdio.h>
#include <epicsString.h>
#include <epicsThread.h>
#include <epicsMutex.h>
#include <epicsTime.h>
#include <cantProceed.h>
#include <errlog.h>
#include <osiSock.h>
#include <iocsh.h>
#include <epicsExport.h>

/* Asyn includes */
#include "asynDriver.h"
#include "asynDrvUser.h"
#include "asynOctetSyncIO.h"
#include "asynCommonSyncIO.h"
#include "asynInt32.h"
#include "asynUInt32Digital.h"
#include "asynInt32Array.h"
#include "asynFloat64.h"

#include "modbusTCP.h"
#include "drvModbusTCPAsyn.h"

/* Defined constants */

#define MAX_READ_WORDS       125        /* Modbus limit on number of words to read */
#define MAX_WRITE_WORDS      123        /* Modbus limit on number of words to write */
#define HISTOGRAM_LENGTH     200        /* Length of time histogram */
#define MAX_TCP_MESSAGE_SIZE 1500       /* Buffer size for input and output packets */
#define MODBUS_READ_TIMEOUT  2.0        /* Timeout for asynOctetSyncIO->writeRead */
#define MIN_POLL_DELAY      .001        /* Minimum polling delay */


/* Structures for drvUser interface */

typedef enum {
    modbusDataCommand,
    modbusEnableHistogramCommand,
    modbusReadHistogramCommand,
    modbusPollDelayCommand,
    modbusReadOKCommand,
    modbusWriteOKCommand,
    modbusIOErrorsCommand,
    modbusLastIOTimeCommand,
    modbusMaxIOTimeCommand
} modbusCommand;

/* Note, this constant must match the number of enums in modbusCommand */
#define MAX_MODBUS_COMMANDS 9

typedef struct {
    modbusCommand command;
    char *commandString;
} modbusCommandStruct;

static modbusCommandStruct modbusCommands[MAX_MODBUS_COMMANDS] = {
    {modbusDataCommand,            MODBUS_DATA_COMMAND_STRING},    
    {modbusEnableHistogramCommand, MODBUS_ENABLE_HISTOGRAM_COMMAND_STRING},
    {modbusReadHistogramCommand,   MODBUS_READ_HISTOGRAM_COMMAND_STRING}, 
    {modbusPollDelayCommand,       MODBUS_POLL_DELAY_COMMAND_STRING}, 
    {modbusReadOKCommand,          MODBUS_READ_OK_COMMAND_STRING}, 
    {modbusWriteOKCommand,         MODBUS_WRITE_OK_COMMAND_STRING}, 
    {modbusIOErrorsCommand,        MODBUS_IO_ERRORS_COMMAND_STRING} ,
    {modbusLastIOTimeCommand,      MODBUS_LAST_IO_TIME_COMMAND_STRING} ,
    {modbusMaxIOTimeCommand,       MODBUS_MAX_IO_TIME_COMMAND_STRING} 
};




/* The structure for the drvModbusTCP asyn port or "object" */

typedef struct modbusTCPStr *PLC_ID;

typedef struct modbusTCPStr
{
    char *portName;             /* asyn port name for this server */
    char *tcpPortName;          /* asyn port name for the asyn TCP port */
    char *plcType;              /* String describing PLC type */
    int isConnected;            /* Connection status */
    int ioStatus;               /* I/O error status */
    asynUser  *pasynUserOctet;  /* asynUser for asynOctet interface to asyn TCP port */ 
    asynUser  *pasynUserTrace;  /* asynUser for asynTrace on this port */
    asynInterface asynCommon;   /* asyn interfaces for this port */
    asynInterface asynDrvUser;
    asynInterface asynUint32D;
    asynInterface asynInt32;
    asynInterface asynInt32Array;
    asynInterface asynFloat64;
    void *asynUInt32DInterruptPvt;  /* Pointers for callbacks */
    void *asynInt32InterruptPvt;
    void *asynFloat64InterruptPvt;
    void *asynInt32ArrayInterruptPvt;
    epicsMutexId mutexId;       /* Mutex for interlocking access to doModbusIO */
    int modbusFunction;         /* Modbus function code */
    int modbusStartAddress;     /* Modbus starting addess for this port */
    int modbusLength;           /* Number of words or bits of Modbus data */
    modbusDataType dataType;    /* Data type */
    unsigned short *data;       /* Memory buffer */
    char modbusRequest[MAX_TCP_MESSAGE_SIZE];      /* Modbus request message */
    char modbusReply[MAX_TCP_MESSAGE_SIZE];        /* Modbus reply message */
    double pollDelay;           /* Delay for readPoller */
    epicsThreadId readPollerThreadId;
    int forceCallback;
    int readOnceFunction;
    int readOnceDone;
    int readOK;                 /* Statistics */
    int writeOK;
    int IOErrors;
    int maxIOMsec;
    int lastIOMsec; 
    epicsInt32 timeHistogram[HISTOGRAM_LENGTH];     /* Histogram of read-times */
    int enableHistogram;
} modbusTCPStr_t;


/* Local variable declarations */
static char *driver = "drvModbusTCPAsyn";           /* String for asynPrint */

/* Local function declarations */

/* These functions are in the asynCommon interface */
static void asynReport              (void *drvPvt, FILE *fp, int details);
static asynStatus asynConnect       (void *drvPvt, asynUser *pasynUser);
static asynStatus asynDisconnect    (void *drvPvt, asynUser *pasynUser);

static asynStatus drvUserCreate     (void *drvPvt, asynUser *pasynUser,
                                     const char *drvInfo,
                                     const char **pptypeName, size_t *psize);
static asynStatus drvUserGetType    (void *drvPvt, asynUser *pasynUser,
                                     const char **pptypeName, size_t *psize);
static asynStatus drvUserDestroy    (void *drvPvt, asynUser *pasynUser);

/* These functions are in the asynUInt32Digital interface */
static asynStatus writeUInt32D      (void *drvPvt, asynUser *pasynUser,
                                     epicsUInt32 value, epicsUInt32 mask);
static asynStatus readUInt32D       (void *drvPvt, asynUser *pasynUser,
                                     epicsUInt32 *value, epicsUInt32 mask);
static asynStatus setInterrupt      (void *drvPvt, asynUser *pasynUser,
                                     epicsUInt32 mask, interruptReason reason);
static asynStatus clearInterrupt    (void *drvPvt, asynUser *pasynUser,
                                     epicsUInt32 mask);
static asynStatus getInterrupt      (void *drvPvt, asynUser *pasynUser,
                                     epicsUInt32 *mask, interruptReason reason);

/* These functions are in the asynInt32 interface */
static asynStatus writeInt32        (void *drvPvt, asynUser *pasynUser,
                                     epicsInt32 value);
static asynStatus readInt32         (void *drvPvt, asynUser *pasynUser,
                                     epicsInt32 *value);
static asynStatus getBounds         (void *drvPvt, asynUser *pasynUser,
                                     epicsInt32 *low, epicsInt32 *high);

/* These functions are in the asynFloat64 interface */
static asynStatus writeFloat64      (void *drvPvt, asynUser *pasynUser,
                                     epicsFloat64 value);
static asynStatus readFloat64       (void *drvPvt, asynUser *pasynUser,
                                     epicsFloat64 *value);

/* These functions are in the asynInt32Array interface */
static asynStatus readInt32Array    (void *drvPvt, asynUser *pasynUser,
                                     epicsInt32 *data, size_t maxChans,
                                     size_t *nactual);
static asynStatus writeInt32Array   (void *drvPvt, asynUser *pasynUser,
                                     epicsInt32 *data, size_t maxChans);

/* These functions are not in any of the asyn interfaces */
static void readPoller(PLC_ID pPlc);
static int doModbusIO(PLC_ID pPlc, int function, int start, unsigned short *data, 
                      int len);
static unsigned short convertToBinary(unsigned short value, modbusDataType dataType);
static unsigned short convertFromBinary(unsigned short value, modbusDataType dataType);


/* asynCommon methods */
static const struct asynCommon drvCommon = {
    asynReport,
    asynConnect,
    asynDisconnect
};

/* asynDrvUser methods */
static asynDrvUser drvUser = {
    drvUserCreate,
    drvUserGetType,
    drvUserDestroy
};

/* asynUInt32Digital methods */
static struct asynUInt32Digital drvUInt32D = {
    writeUInt32D,
    readUInt32D,
    setInterrupt,
    clearInterrupt,
    getInterrupt,
    NULL,
    NULL
};

/* asynInt32 methods */
static asynInt32 drvInt32 = {
    writeInt32,
    readInt32,
    getBounds,
    NULL,
    NULL
};
/* asynFloat64 methods */
static asynFloat64 drvFloat64 = {
    writeFloat64,
    readFloat64,
    NULL,
    NULL
};

/* asynInt32Array methods */
static asynInt32Array drvInt32Array = {
    writeInt32Array,
    readInt32Array,
    NULL,
    NULL
};




/********************************************************************
**  global driver functions
*********************************************************************
*/

/*
** drvModbusTCPAsynConfigure() - create and init an asyn port driver for a PLC
**                                                                    
*/

int drvModbusTCPAsynConfigure(char *portName, 
                              char *tcpPortName, 
                              int modbusFunction, 
                              int modbusStartAddress, 
                              int modbusLength,
                              modbusDataType dataType,
                              int pollMsec, 
                              char *plcType)
{
    int status;
    PLC_ID pPlc;
    char readThreadName[100];
    int needReadThread=0;
    int IOLength=0;
    int maxLength=0;
    int canBlock=0;

    pPlc = callocMustSucceed(1, sizeof(*pPlc), "drvModbusTCPAsynConfigure");
    pPlc->portName = epicsStrDup(portName);
    pPlc->tcpPortName = epicsStrDup(tcpPortName);
    pPlc->plcType = epicsStrDup(plcType);
    pPlc->modbusFunction = modbusFunction;
    pPlc->modbusStartAddress = modbusStartAddress;
    pPlc->modbusLength = modbusLength;
    pPlc->dataType = dataType;
    pPlc->pollDelay = pollMsec/1000.;
    if (pPlc->pollDelay < MIN_POLL_DELAY) pPlc->pollDelay = MIN_POLL_DELAY;
    pPlc->mutexId = epicsMutexMustCreate();

    switch(pPlc->modbusFunction) {
        case MODBUS_READ_COILS:
        case MODBUS_READ_DISCRETE_INPUTS:
            IOLength = pPlc->modbusLength/16;
            maxLength = MAX_READ_WORDS;
            needReadThread = 1;
            break;
        case MODBUS_READ_HOLDING_REGISTERS:
        case MODBUS_READ_INPUT_REGISTERS:
            IOLength = pPlc->modbusLength;
            maxLength = MAX_READ_WORDS;
            needReadThread = 1;
            break;
        case MODBUS_WRITE_SINGLE_COIL:
        case MODBUS_WRITE_MULTIPLE_COILS:
            IOLength = pPlc->modbusLength/16;
            maxLength = MAX_WRITE_WORDS;
            if (pollMsec != 0) pPlc->readOnceFunction = MODBUS_READ_COILS;
            canBlock = ASYN_CANBLOCK;
            break;
       case MODBUS_WRITE_SINGLE_REGISTER:
       case MODBUS_WRITE_MULTIPLE_REGISTERS:
            IOLength = pPlc->modbusLength;
            maxLength = MAX_WRITE_WORDS;
            if (pollMsec != 0) pPlc->readOnceFunction = MODBUS_READ_HOLDING_REGISTERS;
            canBlock = ASYN_CANBLOCK;
            break;
       default:
            errlogPrintf("%s::drvModbusTCPAsynConfig port %s unsupported"
                         " Modbus function %d\n",
                         driver, pPlc->portName, pPlc->modbusFunction);
            return(asynError);
    }
 
    /* Make sure memory length is valid. */
    if (IOLength > maxLength) {
        errlogPrintf("%s::drvModbusTCPConfigure, port %s" 
                     " memory length=%d too large, max=%d\n",
                     driver, pPlc->portName, IOLength, maxLength);
        return(asynError);
    }
    
    /* Note that we always allocate modbusLength words of memory.  
     * This is needed even for write operations because we need a buffer to convert
     * data for asynInt32Array writes. */
    if (pPlc->modbusLength != 0) {
        pPlc->data = callocMustSucceed(pPlc->modbusLength, sizeof(unsigned short), 
                                       "drvModbusTCPAsynConfigure");
    }

    
    /* Connect to TCP asyn port with asynOctetSyncIO */
    status = pasynOctetSyncIO->connect(tcpPortName, 0, &pPlc->pasynUserOctet, 0);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't connect to TCP server %s.\n",
                     driver, portName, tcpPortName);
        return(asynError);
    }

    /* Create asyn interfaces and register with asynManager */
    pPlc->asynCommon.interfaceType = asynCommonType;
    pPlc->asynCommon.pinterface  = (void *)&drvCommon;
    pPlc->asynCommon.drvPvt = pPlc;
    pPlc->asynDrvUser.interfaceType = asynDrvUserType;
    pPlc->asynDrvUser.pinterface  = (void *)&drvUser;
    pPlc->asynDrvUser.drvPvt = pPlc;
    pPlc->asynUint32D.interfaceType = asynUInt32DigitalType;
    pPlc->asynUint32D.pinterface  = (void *)&drvUInt32D;
    pPlc->asynUint32D.drvPvt = pPlc;
    pPlc->asynInt32.interfaceType = asynInt32Type;
    pPlc->asynInt32.pinterface  = (void *)&drvInt32;
    pPlc->asynInt32.drvPvt = pPlc;
    pPlc->asynFloat64.interfaceType = asynFloat64Type;
    pPlc->asynFloat64.pinterface  = (void *)&drvFloat64;
    pPlc->asynFloat64.drvPvt = pPlc;
    pPlc->asynInt32Array.interfaceType = asynInt32ArrayType;
    pPlc->asynInt32Array.pinterface  = (void *)&drvInt32Array;
    pPlc->asynInt32Array.drvPvt = pPlc;

    status = pasynManager->registerPort(pPlc->portName,
                                        ASYN_MULTIDEVICE | canBlock,
                                        1, /* autoconnect */
                                        0, /* medium priority */
                                        0); /* default stack size */
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register port\n",
                     driver, pPlc->portName);
        return(asynError);
    }
    status = pasynManager->registerInterface(pPlc->portName, &pPlc->asynCommon);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynCommon interface.\n",
                     driver, pPlc->portName);
        return(asynError);
    }

    status = pasynManager->registerInterface(pPlc->portName, &pPlc->asynDrvUser);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register drvUser interface.\n",
                     driver, pPlc->portName);
        return(asynError);
    }

    status = pasynUInt32DigitalBase->initialize(pPlc->portName, &pPlc->asynUint32D);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynUInt32D interface.\n",
                     driver, pPlc->portName);
        return(asynError);
    }
    status = pasynManager->registerInterruptSource(pPlc->portName, &pPlc->asynUint32D,
                                                   &pPlc->asynUInt32DInterruptPvt);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynUInt32D interrupt source\n",
                     driver, pPlc->portName);
        return(asynError);
    }

    status = pasynInt32Base->initialize(pPlc->portName, &pPlc->asynInt32);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynInt32 interface.\n",
                     driver, pPlc->portName);
        return(asynError);
    }
    pasynManager->registerInterruptSource(pPlc->portName, &pPlc->asynInt32,
                                          &pPlc->asynInt32InterruptPvt);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynInt32 interrupt source\n",
                     driver, pPlc->portName);
        return(asynError);
    }
    
    status = pasynFloat64Base->initialize(pPlc->portName, &pPlc->asynFloat64);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynFloat64 interface.\n",
                     driver, pPlc->portName);
        return(asynError);
    }
    pasynManager->registerInterruptSource(pPlc->portName, &pPlc->asynFloat64,
                                          &pPlc->asynFloat64InterruptPvt);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynFloat64 interrupt source\n",
                     driver, pPlc->portName);
        return(asynError);
    }

    status = pasynInt32ArrayBase->initialize(pPlc->portName, &pPlc->asynInt32Array);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynInt32Array interface.\n",
                     driver, pPlc->portName);
        return(asynError);
    }
    status = pasynManager->registerInterruptSource(pPlc->portName, 
                                                   &pPlc->asynInt32Array,
                                                   &pPlc->asynInt32ArrayInterruptPvt);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " can't register asynInt32Array interrupt source\n",
                     driver, pPlc->portName);
        return(asynError);
    }

    /* Create asynUser for asynTrace */
    pPlc->pasynUserTrace = pasynManager->createAsynUser(0, 0);
    pPlc->pasynUserTrace->userPvt = pPlc;

    /* Connect to device */
    status = pasynManager->connectDevice(pPlc->pasynUserTrace, pPlc->portName, 0);
    if (status != asynSuccess) {
        errlogPrintf("%s::drvModbusTCPAsynConfigure port %s"
                     " connectDevice failed %s\n",
                     driver, pPlc->portName, pPlc->pasynUserTrace->errorMessage);
         return(asynError);
    }
    
    /* If this is an output function do a readOnce operation if required. */
    if (pPlc->readOnceFunction) {
        status = doModbusIO(pPlc, pPlc->readOnceFunction, pPlc->modbusStartAddress, 
                            pPlc->data, pPlc->modbusLength);
        if (status == asynSuccess) pPlc->readOnceDone = 1;
    }
     
    /* Create the thread to read registers if this is a read function code */
    if (needReadThread) {
        epicsSnprintf(readThreadName, 100, "%sRead", pPlc->portName);
        pPlc->readPollerThreadId = epicsThreadCreate(readThreadName,
           epicsThreadPriorityMedium,
           epicsThreadGetStackSize(epicsThreadStackSmall),
           (EPICSTHREADFUNC)readPoller, 
           pPlc);
        pPlc->forceCallback = 1;
    }

    return(asynSuccess);
}


/* asynDrvUser routines */
static asynStatus drvUserCreate(void *drvPvt, asynUser *pasynUser,
                                const char *drvInfo,
                                const char **pptypeName, size_t *psize)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int i;
    char *pstring;
    
    /* We are passed a string that identifies this command.
     * Set pasynUser->reason based on this string */

    for (i=0; i<MAX_MODBUS_COMMANDS; i++) {
        pstring = modbusCommands[i].commandString;
        if (epicsStrCaseCmp(drvInfo, pstring) == 0) {
            pasynUser->reason = modbusCommands[i].command;
            if (pptypeName) *pptypeName = epicsStrDup(pstring);
            if (psize) *psize = sizeof(modbusCommands[i].command);
            asynPrint(pasynUser, ASYN_TRACE_FLOW,
                      "%s::drvUserCreate, port %s command=%s\n", 
                      driver, pPlc->portName, pstring);
            return(asynSuccess);
        }
    }
    asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s::drvUserCreate, port %s unknown command=%s\n", 
              driver, pPlc->portName, drvInfo);
    return(asynError);
}

static asynStatus drvUserGetType(void *drvPvt, asynUser *pasynUser,
                                 const char **pptypeName, size_t *psize)
{
    int command = pasynUser->reason;

    if (pptypeName)
        *pptypeName = epicsStrDup(modbusCommands[command].commandString);
    if (psize) *psize = sizeof(command);
    return(asynSuccess);
}

static asynStatus drvUserDestroy(void *drvPvt, asynUser *pasynUser)
{
    return(asynSuccess);
}


/***********************/
/* asynCommon routines */
/***********************/

/* Connect */
static asynStatus asynConnect(void *drvPvt, asynUser *pasynUser)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int signal;
  
    pasynManager->getAddr(pasynUser, &signal);
    if (signal < pPlc->modbusLength) {
        pasynManager->exceptionConnect(pasynUser);
        return(asynSuccess);
    } else {
        return(asynError);
    }
}

/* Disconnect */
static asynStatus asynDisconnect(void *drvPvt, asynUser *pasynUser)
{
    /* Does nothing for now.  
     * May be used if connection management is implemented */
    pasynManager->exceptionDisconnect(pasynUser);
    return(asynSuccess);
}


/* Report  parameters */
static void asynReport(void *drvPvt, FILE *fp, int details)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;

    fprintf(fp, "modbusTCP port: %s\n", pPlc->portName);
    if (details) {
        fprintf(fp, "    asyn TCP server:    %s\n", pPlc->tcpPortName);
        fprintf(fp, "    modbusFunction:     %d\n", pPlc->modbusFunction);
        fprintf(fp, "    modbusStartAddress: 0%o\n", pPlc->modbusStartAddress);
        fprintf(fp, "    modbusLength:       0%o\n", pPlc->modbusLength);
        fprintf(fp, "    plcType:            %s\n", pPlc->plcType);
        fprintf(fp, "    I/O errors:         %d\n", pPlc->IOErrors);
        fprintf(fp, "    Read OK:            %d\n", pPlc->readOK);
        fprintf(fp, "    Write OK:           %d\n", pPlc->writeOK);
        fprintf(fp, "    pollDelay:          %f\n", pPlc->pollDelay);
        fprintf(fp, "    Time for last I/O   %d msec\n", pPlc->lastIOMsec);
        fprintf(fp, "    Max. I/O time:      %d msec\n", pPlc->maxIOMsec);
    }

}


/* 
**  asynUInt32D support
*/
static asynStatus readUInt32D(void *drvPvt, asynUser *pasynUser, epicsUInt32 *value,
                              epicsUInt32 mask)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int offset;
    
    
    switch(pasynUser->reason) {
        case modbusDataCommand:
            if (pPlc->ioStatus != asynSuccess) return(pPlc->ioStatus);
            pasynManager->getAddr(pasynUser, &offset);
            *value = 0;
            if (offset >= pPlc->modbusLength) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::readUInt32D port %s invalid memory request %d, max=%d\n",
                          driver, pPlc->portName, offset, pPlc->modbusLength);
                return(asynError);
            }
            switch(pPlc->modbusFunction) {
                case MODBUS_READ_COILS:
                case MODBUS_READ_DISCRETE_INPUTS:
                case MODBUS_READ_HOLDING_REGISTERS:
                case MODBUS_READ_INPUT_REGISTERS:
                    *value = pPlc->data[offset];
                    if ((mask != 0 ) && (mask != 0xFFFF)) *value &= mask;
                    break;
                case MODBUS_WRITE_SINGLE_COIL:
                case MODBUS_WRITE_MULTIPLE_COILS:
                case MODBUS_WRITE_SINGLE_REGISTER:
                case MODBUS_WRITE_MULTIPLE_REGISTERS:
                    if (!pPlc->readOnceDone) return(asynError);
                    *value = pPlc->data[offset];
                    if ((mask != 0 ) && (mask != 0xFFFF)) *value &= mask;
                    break;
                default:
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::readUInt32D port %s invalid request for Modbus"
                              " function %d\n",
                              driver, pPlc->portName, pPlc->modbusFunction);
                    return(asynError);
            }
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER,
                      "%s::readUInt32D port %s function=0x%x,"
                      " offset=0%o, mask=0x%x, value=0x%x\n",
                      driver, pPlc->portName, pPlc->modbusFunction, 
                      offset, mask, *value);
            break;
        case modbusEnableHistogramCommand:
            *value = pPlc->enableHistogram;
            break;
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::writeUInt32D port %s invalid pasynUser->reason %d\n",
                      driver, pPlc->portName, pasynUser->reason);
            return(asynError);
    }

    return(asynSuccess);
}


static asynStatus writeUInt32D(void *drvPvt, asynUser *pasynUser, epicsUInt32 value,
                               epicsUInt32 mask)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int offset;
    int modbusAddress;
    int i;
    unsigned short data = value;
    asynStatus status;
 
    switch(pasynUser->reason) {
        case modbusDataCommand:
            pasynManager->getAddr(pasynUser, &offset);
            modbusAddress = pPlc->modbusStartAddress + offset;
            if (offset >= pPlc->modbusLength) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::writeUInt32D port %s invalid memory request %d, max=%d\n",
                          driver, pPlc->portName, offset, pPlc->modbusLength);
                return(asynError);
            }
            switch(pPlc->modbusFunction) {
                case MODBUS_WRITE_SINGLE_COIL:
                    status = doModbusIO(pPlc, pPlc->modbusFunction, modbusAddress, 
                                        &data, 1);
                    if (status != asynSuccess) return(status);
                    break;
                case MODBUS_WRITE_SINGLE_REGISTER:
                    /* Do this as a read/modify/write if mask is not all 0 or all 1 */
                    if ((mask == 0) || (mask == 0xFFFF)) {
                        status = doModbusIO(pPlc, pPlc->modbusFunction, modbusAddress, 
                                            &data, 1);
                    } else {
                        status = doModbusIO(pPlc, MODBUS_READ_HOLDING_REGISTERS,
                                            modbusAddress, &data, 1);
                        if (status != asynSuccess) return(status);
                        /* Set bits that are set in the value and set in the mask */
                        data |=  (value & mask);
                        /* Clear bits that are clear in the value and set in the mask */
                        data  &= (value | ~mask);
                        status = doModbusIO(pPlc, pPlc->modbusFunction, modbusAddress, 
                                            &data, 1);
                    }
                    if (status != asynSuccess) return(status);
                    break;
                default:
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::writeUInt32D port %s invalid request for Modbus"
                              " function %d\n",
                              driver, pPlc->portName, pPlc->modbusFunction);
                    return(asynError);
            }
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER,
                      "%s::writeUInt32D port %s function=0x%x,"
                      " address=0%o, mask=0x%x, value=0x%x\n",
                      driver, pPlc->portName, pPlc->modbusFunction, 
                      modbusAddress, mask, data);
            break;
        case modbusEnableHistogramCommand:
            if ((value != 0) && pPlc->enableHistogram == 0) {
                /* We are turning on histogram enabling, erase existing data first */
                for (i=0; i<HISTOGRAM_LENGTH; i++) {
                    pPlc->timeHistogram[i] = 0;
                }
            }
            pPlc->enableHistogram = value ? 1 : 0;
            break;
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::writeUInt32D port %s invalid pasynUser->reason %d\n",
                      driver, pPlc->portName, pasynUser->reason);
            return(asynError);
    }
           
    return(asynSuccess);
}


/* setInterrupt, clearInterrupt, and getInterrupt are required by the asynUInt32Digital
 * interface.
 * They are used to control hardware interrupts for drivers that use them.  
 * They are not needed for Modbus, so we just return asynSuccess. */
 
static asynStatus setInterrupt (void *drvPvt, asynUser *pasynUser, epicsUInt32 mask, 
                                interruptReason reason)
{
    return(asynSuccess);
}
                                
static asynStatus clearInterrupt (void *drvPvt, asynUser *pasynUser, epicsUInt32 mask)
{
    return(asynSuccess);
}

static asynStatus getInterrupt (void *drvPvt, asynUser *pasynUser, epicsUInt32 *mask, 
                                interruptReason reason)
{
    return(asynSuccess);
}
                                


/* 
**  asynInt32 support
*/

static asynStatus readInt32 (void *drvPvt, asynUser *pasynUser, epicsInt32 *value)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int offset;
    
    pasynManager->getAddr(pasynUser, &offset);
    *value = 0;
    
    switch(pasynUser->reason) {
        case modbusDataCommand:
            if (pPlc->ioStatus != asynSuccess) return(pPlc->ioStatus);
            if (offset >= pPlc->modbusLength) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::readInt32 port %s invalid memory request %d, max=%d\n",
                          driver, pPlc->portName, offset, pPlc->modbusLength);
                return(asynError);
            }
            switch(pPlc->modbusFunction) {
                case MODBUS_READ_COILS:
                case MODBUS_READ_DISCRETE_INPUTS:
                case MODBUS_READ_HOLDING_REGISTERS:
                case MODBUS_READ_INPUT_REGISTERS:
                    *value = pPlc->data[offset];
                     break;
                case MODBUS_WRITE_SINGLE_COIL:
                case MODBUS_WRITE_MULTIPLE_COILS:
                case MODBUS_WRITE_SINGLE_REGISTER:
                case MODBUS_WRITE_MULTIPLE_REGISTERS:
                    if (!pPlc->readOnceDone) return(asynError);
                    *value = pPlc->data[offset];
                    break;
                default:
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::readInt32 port %s invalid request for Modbus"
                              " function %d\n",
                              driver, pPlc->portName, pPlc->modbusFunction);
                    return(asynError);
            }
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER,
                      "%s::readInt32 port %s function=0x%x,"
                      " offset=0%o, value=0x%x\n",
                      driver, pPlc->portName, pPlc->modbusFunction, 
                      offset, *value);
            break;
        case modbusReadOKCommand: 
            *value = pPlc->readOK;
            break;
        case modbusWriteOKCommand: 
            *value = pPlc->writeOK;
            break;
        case modbusIOErrorsCommand: 
            *value = pPlc->IOErrors;
            break;
        case modbusLastIOTimeCommand: 
            *value = pPlc->lastIOMsec;
            break;
        case modbusMaxIOTimeCommand: 
            *value = pPlc->maxIOMsec;
            break;
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::readInt32 port %s invalid pasynUser->reason %d\n",
                      driver, pPlc->portName, pasynUser->reason);
            return(asynError);
        }
    
    return(asynSuccess);
}


static asynStatus writeInt32(void *drvPvt, asynUser *pasynUser, epicsInt32 value)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int offset;
    int modbusAddress;
    unsigned short data;
    asynStatus status;

    pasynManager->getAddr(pasynUser, &offset);

    switch(pasynUser->reason) {
        case modbusDataCommand:
            data = value;
            if (offset >= pPlc->modbusLength) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::writeInt32 port %s invalid memory request %d, max=%d\n",
                          driver, pPlc->portName, offset, pPlc->modbusLength);
                return(asynError);
            }
            modbusAddress = pPlc->modbusStartAddress + offset;
            switch(pPlc->modbusFunction) {
                case MODBUS_WRITE_SINGLE_COIL:
                case MODBUS_WRITE_SINGLE_REGISTER:
                    status = doModbusIO(pPlc, pPlc->modbusFunction, modbusAddress, 
                                        &data, 1);
                    if (status != asynSuccess) return(status);
                    break;
                default:
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::writeInt32 port %s invalid request for Modbus"
                              " function %d\n",
                              driver, pPlc->portName, pPlc->modbusFunction);
                    return(asynError);
            }
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER,
                      "%s::writeInt32 port %s function=0x%x,"
                      " modbusAddress=0%o, value=0x%x\n",
                      driver, pPlc->portName, pPlc->modbusFunction, 
                      modbusAddress, data);
            break;
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::writeInt32 port %s invalid pasynUser->reason %d\n",
                      driver, pPlc->portName, pasynUser->reason);
            return(asynError);
    }
    return(asynSuccess);
}


/* This function should return the valid range for asynInt32 data.  
 * However, for Modbus devices there is no way for the driver to know what the 
 * valid range is, since the device could be an 8-bit unipolar ADC, 12-bit bipolar DAC,
 * etc.  Just return 0.
 * It is up to device support to correctly interpret the numbers that are returned. */
 
static asynStatus getBounds (void *drvPvt, asynUser *pasynUser, 
                             epicsInt32 *low, epicsInt32 *high)
{
    *high = 0;
    *low = 0;
    return(asynSuccess);
}



/* 
**  asynFloat64 support
*/
static asynStatus readFloat64 (void *drvPvt, asynUser *pasynUser, epicsFloat64 *value)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int offset;
    
    pasynManager->getAddr(pasynUser, &offset);
    *value = 0;
    
    switch(pasynUser->reason) {
        case modbusDataCommand:
            if (pPlc->ioStatus != asynSuccess) return(pPlc->ioStatus);
            if (offset >= pPlc->modbusLength) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::readFloat64 port %s invalid memory request %d, max=%d\n",
                          driver, pPlc->portName, offset, pPlc->modbusLength);
                return(asynError);
            }
            switch(pPlc->modbusFunction) {
                case MODBUS_READ_COILS:
                case MODBUS_READ_DISCRETE_INPUTS:
                case MODBUS_READ_HOLDING_REGISTERS:
                case MODBUS_READ_INPUT_REGISTERS:
                    *value = pPlc->data[offset];
                     break;
                case MODBUS_WRITE_SINGLE_COIL:
                case MODBUS_WRITE_MULTIPLE_COILS:
                case MODBUS_WRITE_SINGLE_REGISTER:
                case MODBUS_WRITE_MULTIPLE_REGISTERS:
                    if (!pPlc->readOnceDone) return(asynError);
                    *value = pPlc->data[offset];
                    break;
                default:
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::readFloat64 port %s invalid request for Modbus"
                              " function %d\n",
                              driver, pPlc->portName, pPlc->modbusFunction);
                    return(asynError);
            }
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER,
                      "%s::readFloat64 port %s function=0x%x,"
                      " offset=0%o, value=%f\n",
                      driver, pPlc->portName, pPlc->modbusFunction, 
                      offset, *value);
            break;
        case modbusPollDelayCommand:
            *value = pPlc->pollDelay;
            break;
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::readFloat64 port %s invalid pasynUser->reason %d\n",
                      driver, pPlc->portName, pasynUser->reason);
            return(asynError);
        }
    
    return(asynSuccess);
}


static asynStatus writeFloat64 (void *drvPvt, asynUser *pasynUser, epicsFloat64 value)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int offset;
    int modbusAddress;
    unsigned short data;
    asynStatus status;

    pasynManager->getAddr(pasynUser, &offset);

    switch(pasynUser->reason) {
        case modbusDataCommand:
            data = (unsigned short)value;
            if (offset >= pPlc->modbusLength) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::writeFloat64 port %s invalid memory request %d, max=%d\n",
                          driver, pPlc->portName, offset, pPlc->modbusLength);
                return(asynError);
            }
            modbusAddress = pPlc->modbusStartAddress + offset;
            switch(pPlc->modbusFunction) {
                case MODBUS_WRITE_SINGLE_COIL:
                case MODBUS_WRITE_SINGLE_REGISTER:
                    status = doModbusIO(pPlc, pPlc->modbusFunction, modbusAddress, 
                                        &data, 1);
                    if (status != asynSuccess) return(status);
                    break;
                default:
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::writeFloat64 port %s invalid request for Modbus"
                              " function %d\n",
                              driver, pPlc->portName, pPlc->modbusFunction);
                    return(asynError);
            }
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER,
                      "%s::writeFloat64 port %s function=0x%x,"
                      " modbusAddress=0%o, value=0x%x\n",
                      driver, pPlc->portName, pPlc->modbusFunction, 
                      modbusAddress, data);
            break;
        case modbusPollDelayCommand:
            if (value < MIN_POLL_DELAY) value = MIN_POLL_DELAY;
            pPlc->pollDelay = value;
            break;
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::writeFloat64 port %s invalid pasynUser->reason %d\n",
                      driver, pPlc->portName, pasynUser->reason);
            return(asynError);
    }
    return(asynSuccess);
}



/* 
**  asynInt32Array support
*/
static asynStatus readInt32Array (void *drvPvt, asynUser *pasynUser, epicsInt32 *data,
                                  size_t maxChans, size_t *nactual)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int nread;
    int i;
    
    switch(pasynUser->reason) {
        case modbusDataCommand:
            if (pPlc->ioStatus != asynSuccess) return(pPlc->ioStatus);
            nread = maxChans;
            if (nread > pPlc->modbusLength) nread = pPlc->modbusLength;
            *nactual = nread;
            switch(pPlc->modbusFunction) {
                case MODBUS_READ_COILS:
                case MODBUS_READ_DISCRETE_INPUTS:
                case MODBUS_READ_HOLDING_REGISTERS:
                case MODBUS_READ_INPUT_REGISTERS:
                    for (i=0; i<nread; i++) {
                        data[i] = pPlc->data[i];
                    }
                    break;
                    
                case MODBUS_WRITE_SINGLE_COIL:
                case MODBUS_WRITE_MULTIPLE_COILS:
                case MODBUS_WRITE_SINGLE_REGISTER:
                case MODBUS_WRITE_MULTIPLE_REGISTERS:
                    if (!pPlc->readOnceDone) return(asynError);
                    for (i=0; i<nread; i++) {
                        data[i] = pPlc->data[i];
                    }
                    break;
                    
                default:
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::readInt32 port %s invalid request for Modbus"
                              " function %d\n",
                              driver, pPlc->portName, pPlc->modbusFunction);
                    return(asynError);
            }
            asynPrintIO(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER, 
                        (char *)pPlc->data, nread*2, 
                        "%s::readInt32Array port %s, function=0x%x",
                        driver, pPlc->portName, pPlc->modbusFunction);
            break;
            
        case modbusReadHistogramCommand:
            nread = maxChans;
            if (nread > HISTOGRAM_LENGTH) nread = HISTOGRAM_LENGTH;
            *nactual = nread;
            for (i=0; i<nread; i++) {
                data[i] = pPlc->timeHistogram[i];
            }
            break;
        
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::readInt32 port %s invalid pasynUser->reason %d\n",
                      driver, pPlc->portName, pasynUser->reason);
            return(asynError);
        }
    
    return(asynSuccess);
}


static asynStatus writeInt32Array (void *drvPvt, asynUser *pasynUser, epicsInt32 *data,
                                   size_t maxChans)
{
    PLC_ID pPlc = (PLC_ID)drvPvt;
    int modbusAddress;
    int nwrite;
    int i;
    asynStatus status;

    switch(pasynUser->reason) {
        case modbusDataCommand:
            modbusAddress = pPlc->modbusStartAddress;
            switch(pPlc->modbusFunction) {
                case MODBUS_WRITE_MULTIPLE_COILS:
                case MODBUS_WRITE_MULTIPLE_REGISTERS:
                    nwrite = maxChans;
                    if (nwrite > pPlc->modbusLength) nwrite = pPlc->modbusLength;
                    /* Need to copy data to local buffer to convert to unsigned short */
                    for (i=0; i<nwrite; i++) {
                        pPlc->data[i] = data[i];
                    }
                    status = doModbusIO(pPlc, pPlc->modbusFunction, modbusAddress, 
                                        pPlc->data, nwrite);
                    if (status != asynSuccess) return(status);
                    break;
                default:
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::writeInt32Array port %s invalid request for Modbus"
                              " function %d\n",
                              driver, pPlc->portName, pPlc->modbusFunction);
                    return(asynError);
            }
            asynPrintIO(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER, 
                        (char *)pPlc->data, nwrite*2, 
                        "%s::writeInt32Array port %s, function=0x%x",
                        driver, pPlc->portName, pPlc->modbusFunction);
            break;
            
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::writeInt32Array port %s invalid pasynUser->reason %d\n",
                      driver, pPlc->portName, pasynUser->reason);
            return(asynError);
    }
    return(asynSuccess);
}



/*
****************************************************************************
** Poller thread for port reads
   One instance spawned per asyn port
****************************************************************************
*/

static void readPoller(PLC_ID pPlc)
{

    ELLLIST *pclientList;
    interruptNode *pnode;
    asynUInt32DigitalInterrupt *pUInt32D;
    asynInt32Interrupt *pInt32;
    asynFloat64Interrupt *pFloat64;
    asynInt32ArrayInterrupt *pInt32Array;
    int offset;
    int anyChanged;
    asynStatus prevIOStatus=asynSuccess;
    int i;
    unsigned short newValue, prevValue, mask;
    epicsUInt32 uInt32Value;
    epicsInt32 int32Value;
    epicsFloat64 float64Value;
    unsigned short *prevData;    /* Previous contents of memory buffer */
    epicsInt32 *int32Data;       /* Buffer used for asynInt32Array callbacks */

    prevData = callocMustSucceed(pPlc->modbusLength, sizeof(unsigned short), 
                                 "drvModbusTCPAsyn::readPoller");
    int32Data = callocMustSucceed(pPlc->modbusLength, sizeof(epicsInt32), 
                                 "drvModbusTCPAsyn::readPoller");

    /* Loop forever */    
    while (1)
    {
        /* Sleep for the poll delay */
        epicsThreadSleep(pPlc->pollDelay);

        /* Read the data */
        pPlc->ioStatus = doModbusIO(pPlc, pPlc->modbusFunction, pPlc->modbusStartAddress, 
                            pPlc->data, pPlc->modbusLength);
        /* If we have an I/O error this time and the previous time, just try again */
        if (pPlc->ioStatus != asynSuccess &&
            pPlc->ioStatus == prevIOStatus) continue;

        /* If the I/O status has changed then force callbacks */
        if (pPlc->ioStatus != prevIOStatus) pPlc->forceCallback = 1;
        
        /* We process callbacks to device support.  
         * Don't do this until EPICS interruptAccept flag is set. */
        if (!interruptAccept) continue;
                            
        /* See if any memory location has actually changed.  
         * If not, no need to do callbacks. */
        anyChanged = memcmp(pPlc->data, prevData, 
                            pPlc->modbusLength*sizeof(unsigned short));
 
        /* See if there are any asynUInt32Digital callbacks registered to be called
         * when data changes.  These callbacks only happen if the value has changed */
        if (pPlc->forceCallback || anyChanged){
            pasynManager->interruptStart(pPlc->asynUInt32DInterruptPvt, &pclientList);
            pnode = (interruptNode *)ellFirst(pclientList);
            while (pnode) {
                pUInt32D = pnode->drvPvt;
                if (pUInt32D->pasynUser->reason != modbusDataCommand) {
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::readPoller port %s invalid pasynUser->reason %d\n",
                              driver, pPlc->portName, pUInt32D->pasynUser->reason);
                    break;
                }
                pasynManager->getAddr(pUInt32D->pasynUser, &offset);
                if (offset >= pPlc->modbusLength) {
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::readPoller port %s invalid memory request %d, max=%d\n",
                              driver, pPlc->portName, offset, pPlc->modbusLength);
                    break;
                }
                mask = pUInt32D->mask;
                newValue = pPlc->data[offset];
                if ((mask != 0 ) && (mask != 0xFFFF)) newValue &= mask;
                prevValue = prevData[offset];
                if ((mask != 0 ) && (mask != 0xFFFF)) prevValue &= mask;
                if (pPlc->forceCallback || (newValue != prevValue)) {
                    uInt32Value = newValue;
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_FLOW,
                              "%s::readPoller, calling client %p"
                              " mask=0x%x, callback=%p, data=0x%x\n",
                              pUInt32D, pUInt32D->mask, pUInt32D->callback, uInt32Value);
                    pUInt32D->callback(pUInt32D->userPvt, pUInt32D->pasynUser,
                                       uInt32Value, pPlc->ioStatus);
                }
                pnode = (interruptNode *)ellNext(&pnode->node);
            }
            pasynManager->interruptEnd(pPlc->asynUInt32DInterruptPvt);
        }
                
        /* See if there are any asynInt32 callbacks registered to be called. 
         * These are called even if the data has not changed, because we could be doing
         * ADC averaging */
        pasynManager->interruptStart(pPlc->asynInt32InterruptPvt, &pclientList);
        pnode = (interruptNode *)ellFirst(pclientList);
        while (pnode) {
            pInt32 = pnode->drvPvt;
            if (pInt32->pasynUser->reason != modbusDataCommand) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::readPoller port %s invalid pasynUser->reason %d\n",
                          driver, pPlc->portName, pInt32->pasynUser->reason);
                break;
            }
            pasynManager->getAddr(pInt32->pasynUser, &offset);
            if (offset >= pPlc->modbusLength) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::readPoller port %s invalid memory request %d, max=%d\n",
                          driver, pPlc->portName, offset, pPlc->modbusLength);
                break;
            }
            int32Value = pPlc->data[offset];
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_FLOW,
                      "%s::readPoller, calling client %p"
                      "callback=%p, data=0x%x\n",
                      pInt32, pInt32->callback, int32Value);
            pInt32->callback(pInt32->userPvt, pInt32->pasynUser,
                             int32Value, pPlc->ioStatus);
            pnode = (interruptNode *)ellNext(&pnode->node);
        }
        pasynManager->interruptEnd(pPlc->asynInt32InterruptPvt);
 
        /* See if there are any asynFloat64 callbacks registered to be called.
         * These are called even if the data has not changed, because we could be doing
         * ADC averaging */
        pasynManager->interruptStart(pPlc->asynFloat64InterruptPvt, &pclientList);
        pnode = (interruptNode *)ellFirst(pclientList);
        while (pnode) {
            pFloat64 = pnode->drvPvt;
            if (pFloat64->pasynUser->reason != modbusDataCommand) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::readPoller port %s invalid pasynUser->reason %d\n",
                          driver, pPlc->portName, pFloat64->pasynUser->reason);
                break;
            }
            pasynManager->getAddr(pFloat64->pasynUser, &offset);
            if (offset >= pPlc->modbusLength) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                          "%s::readPoller port %s invalid memory request %d, max=%d\n",
                          driver, pPlc->portName, offset, pPlc->modbusLength);
                break;
            }
            float64Value = pPlc->data[offset];
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_FLOW,
                      "%s::readPoller, calling client %p"
                      "callback=%p, data=%f\n",
                      pFloat64, pFloat64->callback, float64Value);
            pFloat64->callback(pFloat64->userPvt, pFloat64->pasynUser,
                               float64Value, pPlc->ioStatus);
            pnode = (interruptNode *)ellNext(&pnode->node);
        }
        pasynManager->interruptEnd(pPlc->asynFloat64InterruptPvt);
        
       
        /* See if there are any asynInt32Array callbacks registered to be called.
         * These are only called when data changes */
        if (pPlc->forceCallback || anyChanged){
            pasynManager->interruptStart(pPlc->asynInt32ArrayInterruptPvt, &pclientList);
            pnode = (interruptNode *)ellFirst(pclientList);
            while (pnode) {
                pInt32Array = pnode->drvPvt;
                if (pInt32Array->pasynUser->reason != modbusDataCommand) {
                    asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                              "%s::readPoller port %s invalid pasynUser->reason %d\n",
                              driver, pPlc->portName, pInt32Array->pasynUser->reason);
                    break;
                }
                /* Need to copy data to epicsInt32 buffer for callback */
                for (i=0; i<pPlc->modbusLength; i++) int32Data[i] = pPlc->data[i];
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_FLOW,
                          "%s::readPoller, calling client %p"
                          "callback=%p\n",
                           pInt32Array, pInt32Array->callback);
                pInt32Array->callback(pInt32Array->userPvt, pInt32Array->pasynUser,
                                      int32Data, pPlc->modbusLength, pPlc->ioStatus);
                pnode = (interruptNode *)ellNext(&pnode->node);
            }
            pasynManager->interruptEnd(pPlc->asynInt32ArrayInterruptPvt);
        }

        /* Reset the forceCallback flag */
        pPlc->forceCallback = 0;

        /* Set the previous I/O status */
        prevIOStatus = pPlc->ioStatus;

        /* Copy the new data to the previous data */
        memcpy(prevData, pPlc->data, pPlc->modbusLength*sizeof(unsigned short));
    }
}



static int doModbusIO(PLC_ID pPlc, int function, int start, 
                      unsigned short *data, int len)
{
    modbusReadRequest *readReq;
    modbusReadResponse *readResp;
    modbusWriteSingleRequest *writeSingleReq;
    modbusWriteSingleResponse *writeSingleResp;
    modbusWriteMultipleRequest *writeMultipleReq;
    modbusWriteMultipleResponse *writeMultipleResp;
    modbusExceptionResponse *exceptionResp;
    int requestSize=0;
    int replySize=MAX_TCP_MESSAGE_SIZE;
    unsigned short transactId=1;
    unsigned short cmdLength;
    unsigned char  destId=0xFF;
    unsigned short modbusEncoding=0;
    unsigned char  *pCharIn, *pCharOut;
    unsigned short *pShortIn, *pShortOut;
    unsigned short bitOutput;
    int byteCount;
    asynStatus status=asynSuccess;
    int i;
    epicsTimeStamp startTime, endTime;
    size_t nwrite, nread;
    int eomReason;
    double dT;
    int msec;
    unsigned char mask=0;
    int autoConnect;
 
    /* We need to protect the code in this function with a Mutex, because it uses the 
     * data buffers in the pPlc stucture for the I/O, and that is not thread safe. */
    epicsMutexMustLock(pPlc->mutexId);

    /* If the TCP driver is not set for autoConnect then do connection management ourselves */
    status = pasynManager->isAutoConnect(pPlc->pasynUserOctet, &autoConnect);
    if (!autoConnect) {
        /* See if we are connected */
        status = pasynManager->isConnected(pPlc->pasynUserOctet, &pPlc->isConnected);
         /* If we have an I/O error or are disconnected then disconnect device and reconnect */
        if ((pPlc->ioStatus != asynSuccess) || !pPlc->isConnected) {
            if (pPlc->ioStatus != asynSuccess) 
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR, 
                          "%s::doModbusIO port %s has I/O error\n",
                          driver, pPlc->portName);
            if (!pPlc->isConnected) 
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR, 
                          "%s::doModbusIO port %s is disconnected\n",
                          driver, pPlc->portName);
            status = pasynCommonSyncIO->disconnectDevice(pPlc->pasynUserOctet);
            if (status == asynSuccess) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_FLOW, 
                          "%s::doModbusIO port %s disconnect device OK\n",
                          driver, pPlc->portName);
            } else {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR, 
                          "%s::doModbusIO port %s disconnect error=%s\n",
                          driver, pPlc->portName, pPlc->pasynUserOctet->errorMessage);
            }
            status = pasynCommonSyncIO->connectDevice(pPlc->pasynUserOctet);
            if (status == asynSuccess) {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_FLOW, 
                          "%s::doModbusIO port %s connect device OK\n",
                          driver, pPlc->portName);
            } else {
                asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR, 
                          "%s::doModbusIO port %s connect device error=%s\n",
                          driver, pPlc->portName, pPlc->pasynUserOctet->errorMessage);
                goto done;
            }
        }
    }
        
    /* First build the parts of the message that are independent of the function type */
    readReq = (modbusReadRequest *)pPlc->modbusRequest;
    readReq->mbapHeader.transactId    = htons(transactId);
    readReq->mbapHeader.protocolType  = htons(modbusEncoding);
    readReq->mbapHeader.destId        = destId;

    switch (function) {
        case MODBUS_READ_COILS:
        case MODBUS_READ_DISCRETE_INPUTS:
        case MODBUS_READ_HOLDING_REGISTERS:
        case MODBUS_READ_INPUT_REGISTERS:
            readReq = (modbusReadRequest *)pPlc->modbusRequest;
            cmdLength = sizeof(modbusReadRequest) - sizeof(modbusMBAPHeader) + 1;
            readReq->mbapHeader.cmdLength = htons(cmdLength);
            readReq->fcode = function;
            readReq->startReg = htons((unsigned short)start);
            readReq->numRead = htons((unsigned short)len);
            requestSize = sizeof(modbusReadRequest);
            break;
        case MODBUS_WRITE_SINGLE_COIL:
            writeSingleReq = (modbusWriteSingleRequest *)pPlc->modbusRequest;
            cmdLength = sizeof(modbusWriteSingleRequest) - sizeof(modbusMBAPHeader) + 1;
            writeSingleReq->mbapHeader.cmdLength = htons(cmdLength);
            writeSingleReq->fcode = function;
            writeSingleReq->startReg = htons((unsigned short)start);
            if (*data) bitOutput = 0xFF00;
            else       bitOutput = 0;
            writeSingleReq->data = htons(bitOutput);
            requestSize = sizeof(modbusWriteSingleRequest);
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER, 
                      "%s::doModbusIO port %s WRITE_SINGLE_COIL"
                      " address=0%o value=0x%x\n",
                      driver, pPlc->portName, start, bitOutput);
            break;
        case MODBUS_WRITE_SINGLE_REGISTER:
            writeSingleReq = (modbusWriteSingleRequest *)pPlc->modbusRequest;
            cmdLength = sizeof(modbusWriteSingleRequest) - sizeof(modbusMBAPHeader) + 1;
            writeSingleReq->mbapHeader.cmdLength = htons(cmdLength);
            writeSingleReq->fcode = function;
            writeSingleReq->startReg = htons((unsigned short)start);
            writeSingleReq->data = htons((unsigned short)*data);
            requestSize = sizeof(modbusWriteSingleRequest);
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER, 
                      "%s::doModbusIO port %s WRITE_SINGLE_REGISTER"
                      " address=0%o value=0x%x\n",
                      driver, pPlc->portName, start, *data);
            break;
        case MODBUS_WRITE_MULTIPLE_COILS:
            writeMultipleReq = (modbusWriteMultipleRequest *)pPlc->modbusRequest;
            writeMultipleReq->fcode = function;
            writeMultipleReq->startReg = htons((unsigned short)start);
            /* Pack bits into output */
            pShortIn = (unsigned short *)data;
            pCharOut = (unsigned char *)&writeMultipleReq->data;
            /* Subtract 1 because it will be incremented first time */
            pCharOut--;
            for (i=0; i<len; i++) {
                if (i%8 == 0) {
                    mask = 0x01;
                    pCharOut++;
                    *pCharOut = 0;
                }
                *pCharOut |= ((*pShortIn++ ? 0xFF:0) & mask);
                mask = mask << 1;
            }
            writeMultipleReq->numOutput = htons(len);
            byteCount = pCharOut - writeMultipleReq->data;
            writeMultipleReq->byteCount = byteCount;
            cmdLength = sizeof(modbusWriteMultipleRequest) - sizeof(modbusMBAPHeader) 
                        + byteCount + 1;
            writeMultipleReq->mbapHeader.cmdLength = htons(cmdLength);
            asynPrintIO(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER, 
                        writeMultipleReq->data, byteCount, 
                        "%s::doModbusIO port %s WRITE_MULTIPLE_COILS",
                        driver, pPlc->portName);
            requestSize = sizeof(modbusWriteMultipleRequest) + byteCount - 1;
            break;
        case MODBUS_WRITE_MULTIPLE_REGISTERS:
            writeMultipleReq = (modbusWriteMultipleRequest *)pPlc->modbusRequest;
            cmdLength = sizeof(modbusWriteMultipleRequest) - sizeof(modbusMBAPHeader) 
                        + 2*len + 1;
            writeMultipleReq->mbapHeader.cmdLength = htons(cmdLength);
            writeMultipleReq->fcode = function;
            writeMultipleReq->startReg = htons((unsigned short)start);
            pShortIn = (unsigned short *)data;
            pShortOut = (unsigned short *)&writeMultipleReq->data;
            for (i=0; i<len; i++) {
                *pShortOut++ = htons(*pShortIn++);
            }
            writeMultipleReq->numOutput = htons(len);
            byteCount = 2*len;
            writeMultipleReq->byteCount = htons(byteCount);
            asynPrintIO(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER, 
                        writeMultipleReq->data, byteCount, 
                        "%s::doModbusIO port %s WRITE_MULTIPLE_REGISTERS",
                        driver, pPlc->portName);
            requestSize = sizeof(modbusWriteMultipleRequest) + byteCount - 1;
            break;
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR, 
                      "%s::doModbusIO, port %s unsupported function code %d\n", 
                      driver, pPlc->portName, function);
            status = asynError;
            goto done;
    }

    /* First we do connection stuff */
    /* See if we are connected with pasynManager->isConnected */
    /* If not connected then called asynCommon->connect (or connectDevice?) */

    /* Do the Modbus I/O as a write/read cycle */
    epicsTimeGetCurrent(&startTime);
    status = pasynOctetSyncIO->writeRead(pPlc->pasynUserOctet, 
                                         pPlc->modbusRequest, requestSize,
                                         pPlc->modbusReply, replySize,
                                         MODBUS_READ_TIMEOUT,
                                         &nwrite, &nread, &eomReason);
    epicsTimeGetCurrent(&endTime);
                                         
    if (status != asynSuccess) {
        asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                 "%s::doModbusIO port %s error calling writeRead,"
                 " error=%s, nwrite=%d/%d, nread=%d\n", 
                 driver, pPlc->portName, 
                 pPlc->pasynUserOctet->errorMessage, nwrite, requestSize, nread);
        pPlc->IOErrors++;
        goto done;
    }
               
    dT = epicsTimeDiffInSeconds(&endTime, &startTime);
    msec = (int)(dT*1000. + 0.5);
    pPlc->lastIOMsec = msec;
    if (msec > pPlc->maxIOMsec) pPlc->maxIOMsec = msec;
    if (pPlc->enableHistogram) {
        /* Longer times go in last bin of histogram */
        if (msec >= HISTOGRAM_LENGTH-1) msec = HISTOGRAM_LENGTH-1; 
        pPlc->timeHistogram[msec]++;
    }     

    /* See if there is a Modbus exception */
    readResp = (modbusReadResponse *)pPlc->modbusReply;
    if (readResp->fcode & MODBUS_EXCEPTION_FCN) {
        exceptionResp = (modbusExceptionResponse *)pPlc->modbusReply;
        asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                  "%s::doModbusIO port %s Modbus exception=%d\n", 
                  driver, pPlc->portName, exceptionResp->exception);
        status = asynError;
        goto done;
    }

    /* Make sure the function code in the response is the same as the one 
     * in the request? */

    switch (function) {
        case MODBUS_READ_COILS:
        case MODBUS_READ_DISCRETE_INPUTS:
            pPlc->readOK++;
            readResp = (modbusReadResponse *)pPlc->modbusReply;
            nread = readResp->byteCount;
            pCharIn = (unsigned char *)&readResp->data;
            /* Subtract 1 because it will be incremented first time */
            pCharIn--;
            /* We assume we got len bits back, since we are only told bytes */
            for (i=0; i<len; i++) {
                if (i%8 == 0) {
                    mask = 0x01;
                    pCharIn++;
                }
                data[i] = (*pCharIn & mask) ? 1:0;
                mask = mask << 1;
            }
            asynPrintIO(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER, 
                        (char *)data, len*2, 
                        "%s::doModbusIO port %s READ_COILS",
                        driver, pPlc->portName);
            break;
        case MODBUS_READ_HOLDING_REGISTERS:
        case MODBUS_READ_INPUT_REGISTERS:
            pPlc->readOK++;
            readResp = (modbusReadResponse *)pPlc->modbusReply;
            nread = readResp->byteCount/2;
            pShortIn = (unsigned short *)&readResp->data;
            for (i=0; i<(int)nread; i++) { 
                data[i] = ntohs(pShortIn[i]);
             }
            /* Convert to binary if required */
            if (pPlc->dataType != dataTypeBinary) {
                for (i=0; i<(int)nread; i++) { 
                    data[i] = convertToBinary(data[i], pPlc->dataType);
                }
            }
            asynPrintIO(pPlc->pasynUserTrace, ASYN_TRACEIO_DRIVER, 
                        (char *)data, nread, 
                        "%s::doModbusIO port %s READ_REGISTERS",
                        driver, pPlc->portName);
            break;

        /* We don't do anything with responses to writes for now.  
         * Could add error checking. */
        case MODBUS_WRITE_SINGLE_COIL:
        case MODBUS_WRITE_SINGLE_REGISTER:
            pPlc->writeOK++;
            writeSingleResp = (modbusWriteSingleResponse *)pPlc->modbusReply;
            break;
        case MODBUS_WRITE_MULTIPLE_COILS:
        case MODBUS_WRITE_MULTIPLE_REGISTERS:
            pPlc->writeOK++;
            writeMultipleResp = (modbusWriteMultipleResponse *)pPlc->modbusReply;
            break;
        default:
            asynPrint(pPlc->pasynUserTrace, ASYN_TRACE_ERROR,
                      "%s::doModbusIO, port %s unsupported function code %d\n", 
                      driver, pPlc->portName, function);
            status = asynError;
            goto done;
    }

    done:
    epicsMutexUnlock(pPlc->mutexId);
    return(status);
}


static unsigned short convertToBinary(unsigned short value, 
                                      modbusDataType dataType)
{
    unsigned short result=0;
    int i;
    int mult=1;
    int signMask = 0x8000;
    int negative = 0;
    
    switch (dataType) {
        case dataTypeSignedBCD:
            if (value & signMask) {
                negative=1;
                value &= ~signMask;
            }
        case dataTypeBCD:
            for(i=0; i<4; i++) {
                result += (value & 0xF)*mult;
                mult = mult*10;
                value = value >> 4;
            }
            if (negative) result = -result;
            break;
        
        case dataTypeSignedBinary:
            result = value;
            if (result & signMask) {
                result &= ~signMask;
                result = -result;
            }
            break;
            
        default:
            break;
    }

    return(result);
}


static unsigned short convertFromBinary(unsigned short value,
                                        modbusDataType dataType)
{
    errlogPrintf("%s::convertFromBinary not yet implemented\n", driver);
    return(asynError);
}


/* iocsh functions */

static const iocshArg ConfigureArg0 = {"Port name",            iocshArgString};
static const iocshArg ConfigureArg1 = {"TCP/IP port name",     iocshArgString};
static const iocshArg ConfigureArg2 = {"Modbus function code", iocshArgInt};
static const iocshArg ConfigureArg3 = {"Modbus start address", iocshArgInt};
static const iocshArg ConfigureArg4 = {"Modbus length",        iocshArgInt};
static const iocshArg ConfigureArg5 = {"Data type (0=binary, 1=BCD)", iocshArgInt};
static const iocshArg ConfigureArg6 = {"Poll time (msec)",     iocshArgInt};
static const iocshArg ConfigureArg7 = {"PLC type",             iocshArgString};

static const iocshArg * const drvModbusTCPAsynConfigureArgs[8] = {
	&ConfigureArg0,
	&ConfigureArg1,
	&ConfigureArg2,
	&ConfigureArg3,
	&ConfigureArg4,
	&ConfigureArg5,
        &ConfigureArg6,
        &ConfigureArg7
};

static const iocshFuncDef drvModbusTCPAsynConfigureFuncDef=
                                                    {"drvModbusTCPAsynConfigure", 8,
                                                     drvModbusTCPAsynConfigureArgs};
static void drvModbusTCPAsynConfigureCallFunc(const iocshArgBuf *args)
{
  drvModbusTCPAsynConfigure(args[0].sval, args[1].sval, args[2].ival, args[3].ival, 
                            args[4].ival, args[5].ival, args[6].ival, args[7].sval);
}


static void drvModbusTCPAsynRegister(void)
{
  iocshRegister(&drvModbusTCPAsynConfigureFuncDef,drvModbusTCPAsynConfigureCallFunc);
}

epicsExportRegistrar(drvModbusTCPAsynRegister);
