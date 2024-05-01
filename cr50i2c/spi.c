#include "driver.h"

static ULONG Cr50DebugLevel = 100;
static ULONG Cr50DebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

typedef struct {
	UINT8 body[4];
} spi_frame_header;

static NTSTATUS spi_transaction(
	_In_  PCR50_CONTEXT  pDevice,
	_In_  BOOLEAN readWrite,
	_In_  size_t bytes,
	_In_  UINT32 addr
) {
	NTSTATUS status;
	spi_frame_header header = { 0 };

	status = SpbLockController(&pDevice->SPIContext);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	SpbWriteDataSynchronously(&pDevice->SPIContext, NULL, 0);

	LARGE_INTEGER Interval;
	Interval.QuadPart = -10 * (LONGLONG)1;
	KeDelayExecutionThread(KernelMode, FALSE, &Interval);

	SpbUnlockController(&pDevice->SPIContext);

	Interval.QuadPart = -10 * (LONGLONG)100;
	KeDelayExecutionThread(KernelMode, FALSE, &Interval);

	status = SpbLockController(&pDevice->SPIContext);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	/*
	 * The first byte of the frame header encodes the transaction type
	 * (read or write) and transfer size (set to length - 1), limited to
	 * 64 bytes.
	 */
	header.body[0] = (readWrite ? 0x80 : 0) | 0x40 | (bytes - 1);
	header.body[1] = 0xd4;

	/* The rest of the frame header is the TPM register address. */
	header.body[2] = (addr >> 8) & 0xff;
	header.body[3] = addr & 0xff;

	status = SpbWriteDataSynchronously(&pDevice->SPIContext, header.body, sizeof(header.body));
	if (!NT_SUCCESS(status)) {
		DbgPrint("Failed to write to SPI! 0x%x\n", status);
		SpbUnlockController(&pDevice->SPIContext);
		return status;
	}

	LARGE_INTEGER StartTime;
	KeQuerySystemTimePrecise(&StartTime);

	UINT8 byte = 0;
	do {
		LARGE_INTEGER CurrentTime;
		KeQuerySystemTimePrecise(&CurrentTime);
		if (((CurrentTime.QuadPart - StartTime.QuadPart) / 10) > 100 * 1000) {
			DbgPrint("Timed out waiting for stall bit\n");
			SpbUnlockController(&pDevice->SPIContext);
			return STATUS_IO_TIMEOUT;
		}

		SpbReadDataSynchronously(&pDevice->SPIContext, &byte, sizeof(byte));
	} while (!(byte & 1));
	return STATUS_SUCCESS;
}

NTSTATUS tpm2_write_reg_spi(
	_In_  PCR50_CONTEXT  pDevice,
	_In_  UINT32 regNumber,
	_In_  UINT8* buffer,
	_In_  size_t bytes
) {
	NTSTATUS status = spi_transaction(pDevice, FALSE, bytes, regNumber);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	status = SpbWriteDataSynchronously(&pDevice->SPIContext, buffer, bytes);
	SpbUnlockController(&pDevice->SPIContext);
	return status;
}

NTSTATUS tpm2_read_reg_spi(
	_In_  PCR50_CONTEXT  pDevice,
	_In_  UINT32 regNumber,
	_Out_  UINT8* buffer,
	_In_  size_t bytes
) {
	NTSTATUS status = spi_transaction(pDevice, TRUE, bytes, regNumber);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	status = SpbReadDataSynchronously(&pDevice->SPIContext, buffer, bytes);
	SpbUnlockController(&pDevice->SPIContext);
	return status;
}

static NTSTATUS tpm2_read_access_reg_spi(
	_In_  PCR50_CONTEXT  pDevice,
	UINT8* access
) {
	return tpm2_read_reg_spi(pDevice, TPM_ACCESS(0), access, sizeof(*access));
}

static NTSTATUS tpm2_write_access_reg_spi(
	_In_  PCR50_CONTEXT  pDevice,
	UINT8 cmd
)
{
	return tpm2_write_reg_spi(pDevice, TPM_ACCESS(0), &cmd, sizeof(cmd));
}

static NTSTATUS tpm_cr50_check_locality(PCR50_CONTEXT pDevice) {
	UINT8 mask = TPM_ACCESS_VALID | TPM_ACCESS_ACTIVE_LOCALITY;
	UINT8 buf;

	NTSTATUS status = tpm2_read_access_reg_spi(pDevice, &buf);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if ((buf & mask) == mask) {
		return status;
	}
	Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Invalid locality 0x%x (should be 0x%x)\n", buf & mask, mask);
	return STATUS_INVALID_DEVICE_STATE;
}

void tpm_cr50_spi_release_locality(PCR50_CONTEXT pDevice, BOOLEAN force) {
	UINT8 mask = TPM_ACCESS_VALID | TPM_ACCESS_REQUEST_PENDING;
	UINT8 buf;

	NTSTATUS status = tpm2_read_access_reg_spi(pDevice, &buf);
	if (!NT_SUCCESS(status)) {
		return;
	}

	if (force || (buf & mask) == mask) {
		buf = TPM_ACCESS_ACTIVE_LOCALITY;
		tpm2_write_access_reg_spi(pDevice, buf);
	}
}

NTSTATUS tpm_cr50_spi_request_locality(PCR50_CONTEXT pDevice) {
	UINT8 buf = TPM_ACCESS_REQUEST_USE;

	NTSTATUS status = tpm_cr50_check_locality(pDevice);
	if (NT_SUCCESS(status)) {
		return status;
	}

	status = tpm2_write_access_reg_spi(pDevice, buf);
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

		KeDelayExecutionThread(KernelMode, FALSE, &WaitInterval);
	}
	Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Setting locality timed out 0x%x\n", status);
	return STATUS_TIMEOUT;
}

NTSTATUS tpm_cr50_spi_tis_status(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz) {
	return tpm2_read_reg_spi(pDevice, TPM_STS(0), buf, sz);
}

NTSTATUS tpm_cr50_spi_tis_status_write(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz) {
	return tpm2_write_reg_spi(pDevice, TPM_STS(0), buf, sz);
}

void tpm_cr50_spi_tis_set_ready(PCR50_CONTEXT pDevice)
{
	UINT8 buf[4] = { TPM_STS_COMMAND_READY };

	tpm2_write_reg_spi(pDevice, TPM_STS(0), buf, sizeof(buf));

	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * TPM_CR50_TIMEOUT_SHORT_MS;

	KeDelayExecutionThread(KernelMode, FALSE, &WaitInterval);
}