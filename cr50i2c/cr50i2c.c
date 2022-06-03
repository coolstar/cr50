#define DESCRIPTOR_DEF
#include "driver.h"
#include "stdint.h"

#define bool int
#define MS_IN_US 1000

static ULONG Cr50I2CDebugLevel = 100;
static ULONG Cr50I2CDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS
DriverEntry(
__in PDRIVER_OBJECT  DriverObject,
__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	Cr50I2CPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, Cr50I2CEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
		);

	if (!NT_SUCCESS(status))
	{
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

static NTSTATUS tpm_cr50_i2c_wait_tpm_ready(PCR50I2C_CONTEXT pDevice) {
	LARGE_INTEGER CurrentTime;
	KeQuerySystemTimePrecise(&CurrentTime);

	LARGE_INTEGER Timeout;
	Timeout.QuadPart = CurrentTime.QuadPart + (TIS_SHORT_TIMEOUT * 1000 * 10);
	while (!pDevice->InterruptServiced) {
		KeQuerySystemTimePrecise(&CurrentTime);
		if (CurrentTime.QuadPart > Timeout.QuadPart) {
			Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Timeout waiting for TPM Interrupt\n");
			return STATUS_TIMEOUT;
		}
	}
	KeQuerySystemTimePrecise(&CurrentTime);

	return STATUS_SUCCESS;
}

static NTSTATUS tpm_cr50_i2c_enable_tpm_irq(PCR50I2C_CONTEXT pDevice) {
	pDevice->InterruptServiced = false;
	WdfInterruptEnable(pDevice->Interrupt);
	return STATUS_SUCCESS;
}

static void tpm_cr50_i2c_disable_tpm_irq(PCR50I2C_CONTEXT pDevice) {
	WdfInterruptDisable(pDevice->Interrupt);
}

static NTSTATUS tpm_cr50_i2c_read(
	_In_ PCR50I2C_CONTEXT pDevice,
	uint8_t addr,
	uint8_t *buf,
	size_t len
) {
	NTSTATUS status = tpm_cr50_i2c_enable_tpm_irq(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = SpbWriteDataSynchronously(&pDevice->I2CContext, &addr, sizeof(uint8_t));
	if (!NT_SUCCESS(status)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"tpm_cr50_i2c_read: SpbWriteDataSynchronously failed with status 0x%x\n", status);
		goto out;
	}

	//Wait for TPM to be ready

	status = tpm_cr50_i2c_wait_tpm_ready(pDevice);
	if (!NT_SUCCESS(status)) {
		goto out;
	}

	status = SpbReadDataSynchronously(&pDevice->I2CContext, buf, len);
	if (!NT_SUCCESS(status)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"tpm_cr50_i2c_read: SpbReadDataSynchronously failed with status 0x%x\n", status);
		goto out;
	}

out:
	tpm_cr50_i2c_disable_tpm_irq(pDevice);
	return status;
}

static NTSTATUS tpm_cr50_i2c_write(
	_In_ PCR50I2C_CONTEXT pDevice,
	uint8_t addr,
	uint8_t* buf,
	size_t len
) {
	if (len > TPM_CR50_MAX_BUFSIZE - 1) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Insufficient memory for write\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	pDevice->buf[0] = addr;
	memcpy(pDevice->buf + 1, buf, len);

	NTSTATUS status = tpm_cr50_i2c_enable_tpm_irq(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = SpbWriteDataSynchronously(&pDevice->I2CContext, pDevice->buf, len + 1);
	if (!NT_SUCCESS(status)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"tpm_cr50_i2c_write: SpbWriteDataSynchronously failed with status 0x%x\n", status);
		goto out;
	}

	//Wait for TPM to be ready

	status = tpm_cr50_i2c_wait_tpm_ready(pDevice);
	if (!NT_SUCCESS(status)) {
		goto out;
	}

out:
	tpm_cr50_i2c_disable_tpm_irq(pDevice);
	return status;
}

static NTSTATUS tpm_cr50_check_locality(PCR50I2C_CONTEXT pDevice) {
	uint8_t mask = TPM_ACCESS_VALID | TPM_ACCESS_ACTIVE_LOCALITY;
	uint8_t buf;

	NTSTATUS status = tpm_cr50_i2c_read(pDevice, TPM_I2C_ACCESS(0), &buf, sizeof(buf));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if ((buf & mask) == mask) {
		return status;
	}
	Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Invalid locality 0x%x (should be 0x%x)\n", buf & mask, mask);
	return STATUS_INVALID_DEVICE_STATE;
}

static void tpm_cr50_release_locality(PCR50I2C_CONTEXT pDevice, bool force) {
	uint8_t mask = TPM_ACCESS_VALID | TPM_ACCESS_REQUEST_PENDING;
	uint8_t addr = TPM_I2C_ACCESS(0);
	uint8_t buf;

	NTSTATUS status = tpm_cr50_i2c_read(pDevice, addr, &buf, sizeof(buf));
	if (!NT_SUCCESS(status)) {
		return;
	}

	if (force || (buf & mask) == mask) {
		buf = TPM_ACCESS_ACTIVE_LOCALITY;
		tpm_cr50_i2c_write(pDevice, addr, &buf, sizeof(buf));
	}
}

static NTSTATUS tpm_cr50_request_locality(PCR50I2C_CONTEXT pDevice) {
	uint8_t buf = TPM_ACCESS_REQUEST_USE;

	NTSTATUS status = tpm_cr50_check_locality(pDevice);
	if (NT_SUCCESS(status)) {
		return status;
	}

	status = tpm_cr50_i2c_write(pDevice, TPM_I2C_ACCESS(0), &buf, sizeof(buf));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * TPM_CR50_TIMEOUT_SHORT_MS;

	for (int i = 0; i < 3; i++) {
		status = tpm_cr50_check_locality(pDevice);
		if (NT_SUCCESS(status)) {
			return status;
		}

		KeDelayExecutionThread(KernelMode, false, &WaitInterval);
	}
	Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Setting locality timed out 0x%x\n", status);
	return STATUS_TIMEOUT;
}

static uint8_t tpm_cr50_i2c_tis_status(PCR50I2C_CONTEXT pDevice) {
	uint8_t buf[4];

	if (!NT_SUCCESS(tpm_cr50_i2c_read(pDevice, TPM_I2C_STS(0), buf, sizeof(buf))))
		return 0;

	return buf[0];
}

static void tpm_cr50_i2c_tis_set_ready(PCR50I2C_CONTEXT pDevice)
{
	uint8_t buf[4] = { TPM_STS_COMMAND_READY };

	tpm_cr50_i2c_write(pDevice, TPM_I2C_STS(0), buf, sizeof(buf));

	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * TPM_CR50_TIMEOUT_SHORT_MS;

	KeDelayExecutionThread(KernelMode, false, &WaitInterval);
}


static NTSTATUS tpm_cr50_i2c_get_burst_and_status(PCR50I2C_CONTEXT pDevice, uint8_t mask,
	size_t *burst, uint32_t *status) {
	LARGE_INTEGER StopTime;
	
	LARGE_INTEGER CurrentTime;
	KeQuerySystemTimePrecise(&CurrentTime);

	uint8_t buf[4];
	*status = 0;

	StopTime.QuadPart = CurrentTime.QuadPart + (10 * 1000 * TIS_LONG_TIMEOUT);
	while (CurrentTime.QuadPart < StopTime.QuadPart) {
		NTSTATUS ret = tpm_cr50_i2c_read(pDevice, TPM_I2C_STS(0), buf, sizeof(buf));
		LARGE_INTEGER WaitInterval;
		WaitInterval.QuadPart = -10 * 1000 * TPM_CR50_TIMEOUT_SHORT_MS;

		if (!NT_SUCCESS(ret)) {
			KeDelayExecutionThread(KernelMode, false, &WaitInterval);

			KeQuerySystemTimePrecise(&CurrentTime);
			continue;
		}

		*status = *buf;
		*burst = *((uint16_t*)(buf + 1));

		if ((*status & mask) == mask && *burst > 0 && *burst <= TPM_CR50_MAX_BUFSIZE - 1)
			return STATUS_SUCCESS;

		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"burst/mask, status: 0x%x, mask: 0x%x, burst: %lld\n", *status & mask, mask, *burst);

		KeDelayExecutionThread(KernelMode, false, &WaitInterval);
		KeQuerySystemTimePrecise(&CurrentTime);
	}

	Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Timeout reading burst and status\n",);
	return STATUS_TIMEOUT;
}

static NTSTATUS tpm_cr50_i2c_tis_recv(PCR50I2C_CONTEXT pDevice, uint8_t* buf, size_t buf_len) {
	uint8_t mask = TPM_STS_VALID | TPM_STS_DATA_AVAIL;
	size_t burstcnt, cur, len, expected;
	uint8_t addr = TPM_I2C_DATA_FIFO(0);
	uint32_t status;
	NTSTATUS ret;

	if (buf_len < TPM_HEADER_SIZE) {
		return STATUS_INVALID_BUFFER_SIZE;
	}

	ret = tpm_cr50_i2c_get_burst_and_status(pDevice, mask, &burstcnt, &status);
	if (!NT_SUCCESS(ret)) {
		goto out_err;
	}

	if (burstcnt > buf_len || burstcnt < TPM_HEADER_SIZE) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Unexpected burstcnt: %zu (max=%zu, min=%d)\n",
			burstcnt, buf_len, TPM_HEADER_SIZE);
		ret = STATUS_IO_DEVICE_ERROR;
		goto out_err;
	}

	/* Read first chunk of burstcnt bytes */
	ret = tpm_cr50_i2c_read(pDevice, addr, buf, burstcnt);
	if (!NT_SUCCESS(ret)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Read of first chunk failed\n");
		goto out_err;
	}

	expected = RtlUlongByteSwap(*((uint32_t*)(buf + 2)));
	if (expected > buf_len) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Buffer too small to receive i2c data\n");
		ret = STATUS_BUFFER_TOO_SMALL;
		goto out_err;
	}

	/* Now read the rest of the data */
	cur = burstcnt;
	while (cur < expected) {
		ret = tpm_cr50_i2c_get_burst_and_status(pDevice, mask, &burstcnt, &status);
		if (!NT_SUCCESS(ret)) {
			goto out_err;
		}

		len = min((size_t)(burstcnt), (size_t)(expected - cur));
		ret = tpm_cr50_i2c_read(pDevice, addr, buf + cur, len);
		if (!NT_SUCCESS(ret)) {
			Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Read failed\n");
			goto out_err;
		}

		cur += len;
	}

	/* Ensure TPM is done reading data */
	ret = tpm_cr50_i2c_get_burst_and_status(pDevice, TPM_STS_VALID, &burstcnt, &status);
	if (!NT_SUCCESS(ret)) {
		goto out_err;
	}

	if (status & TPM_STS_DATA_AVAIL) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Data still available\n");
		ret = IO_ERROR_IO_HARDWARE_ERROR;
		goto out_err;
	}

	tpm_cr50_release_locality(pDevice, false);
	return ret;

out_err:
	if (tpm_cr50_i2c_tis_status(pDevice) & TPM_STS_COMMAND_READY)
		tpm_cr50_i2c_tis_set_ready(pDevice);

	tpm_cr50_release_locality(pDevice, false);
	return ret;
}

static NTSTATUS tpm_cr50_i2c_tis_send(PCR50I2C_CONTEXT pDevice, uint8_t* buf, size_t len) {
	size_t burstcnt, limit, sent = 0;
	uint8_t tpm_go[4] = { TPM_STS_GO };
	uint32_t status;
	NTSTATUS ret;

	ret = tpm_cr50_request_locality(pDevice);
	if (!NT_SUCCESS(ret))
		return ret;

	/* Wait until TPM is ready for a command */
	LARGE_INTEGER StopTime;

	LARGE_INTEGER CurrentTime;
	KeQuerySystemTimePrecise(&CurrentTime);
	StopTime.QuadPart = CurrentTime.QuadPart + (10 * 1000 * TIS_LONG_TIMEOUT);

	while (!(tpm_cr50_i2c_tis_status(pDevice) & TPM_STS_COMMAND_READY)) {
		KeQuerySystemTimePrecise(&CurrentTime);
		if (CurrentTime.QuadPart > StopTime.QuadPart) {
			ret = STATUS_TIMEOUT;
			goto out_err;
		}

		tpm_cr50_i2c_tis_set_ready(pDevice);
	}

	while (len > 0) {
		uint8_t mask = TPM_STS_VALID;

		/* Wait for data if this is not the first chunk */
		if (sent > 0)
			mask |= TPM_STS_DATA_EXPECT;

		/* Read burst count and check status */
		ret = tpm_cr50_i2c_get_burst_and_status(pDevice, mask, &burstcnt, &status);
		if (!NT_SUCCESS(ret))
			goto out_err;

		/*
		 * Use burstcnt - 1 to account for the address byte
		 * that is inserted by tpm_cr50_i2c_write()
		 */
		limit = min((size_t)(burstcnt - 1), (size_t)(len));
		ret = tpm_cr50_i2c_write(pDevice, TPM_I2C_DATA_FIFO(0), &buf[sent], limit);
		if (!NT_SUCCESS(ret)) {
			Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Write failed\n");
			goto out_err;
		}

		sent += limit;
		len -= limit;
	}

	/* Ensure TPM is not expecting more data */
	ret = tpm_cr50_i2c_get_burst_and_status(pDevice, TPM_STS_VALID, &burstcnt, &status);
	if (!NT_SUCCESS(ret))
		goto out_err;
	if (status & TPM_STS_DATA_EXPECT) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Data still expected\n");
		ret = IO_ERROR_IO_HARDWARE_ERROR;
		goto out_err;
	}

	/* Start the TPM command */
	ret = tpm_cr50_i2c_write(pDevice, TPM_I2C_STS(0), tpm_go,
		sizeof(tpm_go));
	if (!NT_SUCCESS(ret)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Start command failed\n");
		goto out_err;
	}
	return STATUS_SUCCESS;

out_err:
	/* Abort current transaction if still pending */
	if (tpm_cr50_i2c_tis_status(pDevice) & TPM_STS_COMMAND_READY)
		tpm_cr50_i2c_tis_set_ready(pDevice);

	tpm_cr50_release_locality(pDevice, false);
	return ret;
}

/**
 * tpm_cr50_i2c_req_canceled() - Callback to notify a request cancel.
 * @chip:	A TPM chip.
 * @status:	Status given by the cancel callback.
 *
 * Return:
 *	True if command is ready, False otherwise.
 */
static bool tpm_cr50_i2c_req_canceled(PCR50I2C_CONTEXT pDevice, uint8_t status)
{
	UNREFERENCED_PARAMETER(pDevice);
	return status == TPM_STS_COMMAND_READY;
}

NTSTATUS InitializeCR50(
	_In_  PCR50I2C_CONTEXT  pDevice
	)
{
	NTSTATUS status = 0;
	uint32_t vendor;
	uint8_t buf[4];

	status = tpm_cr50_request_locality(pDevice);
	if (!NT_SUCCESS(status)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Could not request locality\n");
		return status;
	}

	/* Read four bytes from DID_VID register */
	status = tpm_cr50_i2c_read(pDevice, TPM_I2C_DID_VID(0), buf, sizeof(buf));
	if (!NT_SUCCESS(status)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Could not read vendor id\n");
		tpm_cr50_release_locality(pDevice, true);
		return status;
	}

	vendor = *((uint32_t*)buf);
	if (vendor != TPM_CR50_I2C_DID_VID && vendor != TPM_TI50_I2C_DID_VID) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Vendor ID did not match! ID was %08x\n", vendor);
		tpm_cr50_release_locality(pDevice, true);
		return STATUS_DEVICE_FEATURE_NOT_SUPPORTED;
	}

	DbgPrint("%s TPM 2.0 (id 0x%x)\n",
		vendor == TPM_TI50_I2C_DID_VID ? "ti50" : "cr50",
		vendor >> 16);

	return status;
}

NTSTATUS ReleaseCR50(
	_In_  PCR50I2C_CONTEXT  pDevice
	)
{
	tpm_cr50_release_locality(pDevice, true);
	return STATUS_SUCCESS;
}

NTSTATUS
OnPrepareHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesRaw,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PCR50I2C_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
		return status;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	pDevice->buf = ExAllocatePoolWithTag(NonPagedPool, TPM_CR50_MAX_BUFSIZE, CR50I2C_POOL_TAG);
	if (!pDevice->buf) {
		return STATUS_MEMORY_NOT_ALLOCATED;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PCR50I2C_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	if (pDevice->buf) {
		ExFreePoolWithTag(pDevice->buf, CR50I2C_POOL_TAG);
	}

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	return status;
}

NTSTATUS
OnD0Entry(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PCR50I2C_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	status = InitializeCR50(pDevice);

	return status;
}

NTSTATUS
OnD0Exit(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxTargetState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxTargetState - target power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxTargetState);

	PCR50I2C_CONTEXT pDevice = GetDeviceContext(FxDevice);

	NTSTATUS status = STATUS_SUCCESS;

	uint8_t shutdown_cmd[] = {
		0x80, 0x01,		/* TPM_ST_COMMAND_TAG (0x8001) */
		0, 0, 0, 12,	/* Length in bytes */
		0, 0, 0x01, 0x45,	/* TPM_CC_Shutdown (0x145) */
		0x00, 0x01
	};
	status = tpm_cr50_i2c_tis_send(pDevice, shutdown_cmd, sizeof(shutdown_cmd));
	if (!NT_SUCCESS(status)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Failed to send TPM shutdown command\n");
		return status;
	}

	uint8_t shutdown_response[TPM_HEADER_SIZE];
	status = tpm_cr50_i2c_tis_recv(pDevice, shutdown_response, TPM_HEADER_SIZE);
	if (!NT_SUCCESS(status)) {
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Failed to receive TPM shutdown response\n");
		return status;
	}

	uint8_t expected_response[] = {
		0x80, 0x01,		/* TPM_ST_COMMAND_TAG (0x8001) */
		0, 0, 0, 10,
		0, 0, 0, 0
	};
	if (memcmp(shutdown_response, expected_response, TPM_HEADER_SIZE) != 0) {
		return STATUS_TPM_FAIL;
	}

	status = ReleaseCR50(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	return status;
}

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID) {
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PCR50I2C_CONTEXT pDevice = GetDeviceContext(Device);

	pDevice->InterruptServiced = true;

	return true;
}

NTSTATUS
Cr50I2CEvtDeviceAdd(
IN WDFDRIVER       Driver,
IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDF_INTERRUPT_CONFIG interruptConfig;
	WDFQUEUE                      queue;
	UCHAR                         minorFunction;
	PCR50I2C_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	Cr50I2CPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"Cr50I2CEvtDeviceAdd called\n");

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Because we are a virtual device the root enumerator would just put null values 
	// in response to IRP_MN_QUERY_ID. Lets override that.
	//

	minorFunction = IRP_MN_QUERY_ID;

	status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
		DeviceInit,
		Cr50I2CEvtWdmPreprocessMnQueryId,
		IRP_MJ_PNP,
		&minorFunction,
		1
		);
	if (!NT_SUCCESS(status))
	{
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceInitAssignWdmIrpPreprocessCallback failed Status 0x%x\n", status);

		return status;
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CR50I2C_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = Cr50I2CEvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
		);

	if (!NT_SUCCESS(status))
	{
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
		);

	if (!NT_SUCCESS(status))
	{
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create an interrupt object for hardware notifications
	//
	WDF_INTERRUPT_CONFIG_INIT(
		&interruptConfig,
		OnInterruptIsr,
		NULL);
	interruptConfig.PassiveHandling = TRUE;

	status = WdfInterruptCreate(
		device,
		&interruptConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->Interrupt);

	if (!NT_SUCCESS(status))
	{
		Cr50I2CPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"Error creating WDF interrupt object - %!STATUS!",
			status);

		return status;
	}

	WdfInterruptDisable(devContext->Interrupt);

	devContext->FxDevice = device;

	return status;
}

NTSTATUS
Cr50I2CEvtWdmPreprocessMnQueryId(
WDFDEVICE Device,
PIRP Irp
)
{
	NTSTATUS            status;
	PIO_STACK_LOCATION  IrpStack, previousSp;
	PDEVICE_OBJECT      DeviceObject;
	PWCHAR              buffer;

	PAGED_CODE();

	//
	// Get a pointer to the current location in the Irp
	//

	IrpStack = IoGetCurrentIrpStackLocation(Irp);

	//
	// Get the device object
	//
	DeviceObject = WdfDeviceWdmGetDeviceObject(Device);


	Cr50I2CPrint(DEBUG_LEVEL_VERBOSE, DBG_PNP,
		"Cr50I2CEvtWdmPreprocessMnQueryId Entry\n");

	//
	// This check is required to filter out QUERY_IDs forwarded
	// by the HIDCLASS for the parent FDO. These IDs are sent
	// by PNP manager for the parent FDO if you root-enumerate this driver.
	//
	previousSp = ((PIO_STACK_LOCATION)((UCHAR *)(IrpStack)+
		sizeof(IO_STACK_LOCATION)));

	if (previousSp->DeviceObject == DeviceObject)
	{
		//
		// Filtering out this basically prevents the Found New Hardware
		// popup for the root-enumerated Cr50I2C on reboot.
		//
		status = Irp->IoStatus.Status;
	}
	else
	{
		switch (IrpStack->Parameters.QueryId.IdType)
		{
		case BusQueryDeviceID:
		case BusQueryHardwareIDs:
			//
			// HIDClass is asking for child deviceid & hardwareids.
			// Let us just make up some id for our child device.
			//
			buffer = (PWCHAR)ExAllocatePoolWithTag(
				NonPagedPool,
				CR50I2C_HARDWARE_IDS_LENGTH,
				CR50I2C_POOL_TAG
				);

			if (buffer)
			{
				//
				// Do the copy, store the buffer in the Irp
				//
				RtlCopyMemory(buffer,
					CR50I2C_HARDWARE_IDS,
					CR50I2C_HARDWARE_IDS_LENGTH
					);

				Irp->IoStatus.Information = (ULONG_PTR)buffer;
				status = STATUS_SUCCESS;
			}
			else
			{
				//
				//  No memory
				//
				status = STATUS_INSUFFICIENT_RESOURCES;
			}

			Irp->IoStatus.Status = status;
			//
			// We don't need to forward this to our bus. This query
			// is for our child so we should complete it right here.
			// fallthru.
			//
			IoCompleteRequest(Irp, IO_NO_INCREMENT);

			break;

		default:
			status = Irp->IoStatus.Status;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			break;
		}
	}

	Cr50I2CPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Cr50I2CEvtWdmPreprocessMnQueryId Exit = 0x%x\n", status);

	return status;
}

VOID
Cr50I2CEvtInternalDeviceControl(
IN WDFQUEUE     Queue,
IN WDFREQUEST   Request,
IN size_t       OutputBufferLength,
IN size_t       InputBufferLength,
IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PCR50I2C_CONTEXT     devContext;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	switch (IoControlCode)
	{
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	WdfRequestComplete(Request, status);

	return;
}
