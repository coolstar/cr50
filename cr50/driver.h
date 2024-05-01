#if !defined(_CR50_H_)
#define _CR50_H_
#define POOL_ZERO_DOWN_LEVEL_SUPPORT

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

#include "cr50.h"
#include "spb.h"

//
// String definitions
//

#define DRIVERNAME                 "cr50i2c.sys: "

#define CR50_POOL_TAG            (ULONG) 'CR50'
#define CR50_HARDWARE_IDS        L"CoolStar\\GOOG0005\0\0"
#define CR50_HARDWARE_IDS_LENGTH sizeof(CR50_HARDWARE_IDS)

#define NTDEVICE_NAME_STRING       L"\\Device\\GOOG0005"
#define SYMBOLIC_NAME_STRING       L"\\DosDevices\\GOOG0005"

typedef enum {
	CR50_TRANSPORT_I2C,
	CR50_TRANSPORT_SPI
} CR50_TRANSPORT;

typedef struct _CR50_CONTEXT
{

	//
	// Handle back to the WDFDEVICE
	//

	WDFDEVICE FxDevice;

	WDFQUEUE ReportQueue;

	CR50_TRANSPORT Transport;

	SPB_CONTEXT I2CContext;
	SPB_CONTEXT SPIContext;

	WDFINTERRUPT Interrupt;

	BOOLEAN InterruptServiced;

	char* buf;

} CR50_CONTEXT, *PCR50_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CR50_CONTEXT, GetDeviceContext)

//
// Function definitions
//

DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD Cr50DriverUnload;

EVT_WDF_DRIVER_DEVICE_ADD Cr50EvtDeviceAdd;

EVT_WDFDEVICE_WDM_IRP_PREPROCESS Cr50EvtWdmPreprocessMnQueryId;

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL Cr50EvtInternalDeviceControl;

void tpm_cr50_release_locality(PCR50_CONTEXT pDevice, BOOLEAN force);
NTSTATUS tpm_cr50_request_locality(PCR50_CONTEXT pDevice);
NTSTATUS tpm_cr50_tis_status(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz);
NTSTATUS tpm_cr50_tis_status_write(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz);
void tpm_cr50_tis_set_ready(PCR50_CONTEXT pDevice);

NTSTATUS tpm_cr50_tis_read_data_fifo(PCR50_CONTEXT pDevice, UINT8* buf, size_t burstcnt);
NTSTATUS tpm_cr50_tis_write_data_fifo(PCR50_CONTEXT pDevice, UINT8* buf, size_t burstcnt);

NTSTATUS tpm_cr50_read_vendor(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz);

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
#define Cr50Print(dbglevel, dbgcatagory, fmt, ...) {          \
    if (Cr50DebugLevel >= dbglevel &&                         \
        (Cr50DebugCatagories && dbgcatagory))                 \
		    {                                                           \
        DbgPrint(DRIVERNAME);                                   \
        DbgPrint(fmt, __VA_ARGS__);                             \
		    }                                                           \
}
#else
#define Cr50Print(dbglevel, fmt, ...) {                       \
}
#endif
#endif