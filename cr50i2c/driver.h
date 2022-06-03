#if !defined(_CR50I2C_H_)
#define _CR50I2C_H_

#pragma warning(disable:4200)  // suppress nameless struct/union warning
#pragma warning(disable:4201)  // suppress nameless struct/union warning
#pragma warning(disable:4214)  // suppress bit field types other than int warning
#include <initguid.h>
#include <wdm.h>

#pragma warning(default:4200)
#pragma warning(default:4201)
#pragma warning(default:4214)
#include <wdf.h>

#pragma warning(disable:4201)  // suppress nameless struct/union warning
#pragma warning(disable:4214)  // suppress bit field types other than int warning
#include <hidport.h>

#include "cr50i2c.h"
#include "spb.h"

//
// String definitions
//

#define DRIVERNAME                 "cr50i2c.sys: "

#define CR50I2C_POOL_TAG            (ULONG) 'CR50'
#define CR50I2C_HARDWARE_IDS        L"CoolStar\\GOOG0005\0\0"
#define CR50I2C_HARDWARE_IDS_LENGTH sizeof(CR50I2C_HARDWARE_IDS)

#define NTDEVICE_NAME_STRING       L"\\Device\\GOOG0005"
#define SYMBOLIC_NAME_STRING       L"\\DosDevices\\GOOG0005"

#define true 1
#define false 0

typedef struct _CR50I2C_CONTEXT
{

	//
	// Handle back to the WDFDEVICE
	//

	WDFDEVICE FxDevice;

	WDFQUEUE ReportQueue;

	SPB_CONTEXT I2CContext;

	WDFINTERRUPT Interrupt;

	BOOLEAN InterruptServiced;

	char* buf;

} CR50I2C_CONTEXT, *PCR50I2C_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CR50I2C_CONTEXT, GetDeviceContext)

//
// Function definitions
//

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD Cr50I2CDriverUnload;

EVT_WDF_DRIVER_DEVICE_ADD Cr50I2CEvtDeviceAdd;

EVT_WDFDEVICE_WDM_IRP_PREPROCESS Cr50I2CEvtWdmPreprocessMnQueryId;

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL Cr50I2CEvtInternalDeviceControl;

//
// Helper macros
//

#define DEBUG_LEVEL_ERROR   1
#define DEBUG_LEVEL_INFO    2
#define DEBUG_LEVEL_VERBOSE 3

#define DBG_INIT  1
#define DBG_PNP   2
#define DBG_IOCTL 4

#if 0
#define Cr50I2CPrint(dbglevel, dbgcatagory, fmt, ...) {          \
    if (Cr50I2CDebugLevel >= dbglevel &&                         \
        (Cr50I2CDebugCatagories && dbgcatagory))                 \
		    {                                                           \
        DbgPrint(DRIVERNAME);                                   \
        DbgPrint(fmt, __VA_ARGS__);                             \
		    }                                                           \
}
#else
#define Cr50I2CPrint(dbglevel, fmt, ...) {                       \
}
#endif
#endif