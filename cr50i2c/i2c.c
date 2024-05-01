#include "driver.h"

static ULONG Cr50DebugLevel = 100;
static ULONG Cr50DebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

static NTSTATUS tpm_cr50_i2c_wait_tpm_ready(PCR50_CONTEXT pDevice) {
	LARGE_INTEGER CurrentTime;
	KeQuerySystemTimePrecise(&CurrentTime);

	LARGE_INTEGER Timeout;
	Timeout.QuadPart = CurrentTime.QuadPart + (TIS_SHORT_TIMEOUT * 1000 * 10);
	while (!pDevice->InterruptServiced) {
		KeQuerySystemTimePrecise(&CurrentTime);
		if (CurrentTime.QuadPart > Timeout.QuadPart) {
			Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Timeout waiting for TPM Interrupt\n");
			return STATUS_TIMEOUT;
		}
	}
	KeQuerySystemTimePrecise(&CurrentTime);

	return STATUS_SUCCESS;
}

static NTSTATUS tpm_cr50_i2c_enable_tpm_irq(PCR50_CONTEXT pDevice) {
	pDevice->InterruptServiced = FALSE;
	WdfInterruptEnable(pDevice->Interrupt);
	return STATUS_SUCCESS;
}

static void tpm_cr50_i2c_disable_tpm_irq(PCR50_CONTEXT pDevice) {
	WdfInterruptDisable(pDevice->Interrupt);
}

NTSTATUS tpm_cr50_i2c_read(
	_In_ PCR50_CONTEXT pDevice,
	UINT8 addr,
	UINT8* buf,
	size_t len
) {
	NTSTATUS status = tpm_cr50_i2c_enable_tpm_irq(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = SpbWriteDataSynchronously(&pDevice->I2CContext, &addr, sizeof(UINT8));
	if (!NT_SUCCESS(status)) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
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
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"tpm_cr50_i2c_read: SpbReadDataSynchronously failed with status 0x%x\n", status);
		goto out;
	}

out:
	tpm_cr50_i2c_disable_tpm_irq(pDevice);
	return status;
}

NTSTATUS tpm_cr50_i2c_write(
	_In_ PCR50_CONTEXT pDevice,
	UINT8 addr,
	UINT8* buf,
	size_t len
) {
	if (len > TPM_CR50_MAX_BUFSIZE - 1) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
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
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
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

static NTSTATUS tpm_cr50_check_locality(PCR50_CONTEXT pDevice) {
	UINT8 mask = TPM_ACCESS_VALID | TPM_ACCESS_ACTIVE_LOCALITY;
	UINT8 buf;

	NTSTATUS status = tpm_cr50_i2c_read(pDevice, TPM_I2C_ACCESS(0), &buf, sizeof(buf));
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

void tpm_cr50_i2c_release_locality(PCR50_CONTEXT pDevice, BOOLEAN force) {
	UINT8 mask = TPM_ACCESS_VALID | TPM_ACCESS_REQUEST_PENDING;
	UINT8 addr = TPM_I2C_ACCESS(0);
	UINT8 buf;

	NTSTATUS status = tpm_cr50_i2c_read(pDevice, addr, &buf, sizeof(buf));
	if (!NT_SUCCESS(status)) {
		return;
	}

	if (force || (buf & mask) == mask) {
		buf = TPM_ACCESS_ACTIVE_LOCALITY;
		tpm_cr50_i2c_write(pDevice, addr, &buf, sizeof(buf));
	}
}

NTSTATUS tpm_cr50_i2c_request_locality(PCR50_CONTEXT pDevice) {
	UINT8 buf = TPM_ACCESS_REQUEST_USE;

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

		KeDelayExecutionThread(KernelMode, FALSE, &WaitInterval);
	}
	Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Setting locality timed out 0x%x\n", status);
	return STATUS_TIMEOUT;
}

NTSTATUS tpm_cr50_i2c_tis_status(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz) {
	return tpm_cr50_i2c_read(pDevice, TPM_I2C_STS(0), buf, sz);
}

NTSTATUS tpm_cr50_i2c_tis_status_write(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz) {
	return tpm_cr50_i2c_write(pDevice, TPM_I2C_STS(0), buf, sz);
}

void tpm_cr50_i2c_tis_set_ready(PCR50_CONTEXT pDevice)
{
	UINT8 buf[4] = { TPM_STS_COMMAND_READY };

	tpm_cr50_i2c_write(pDevice, TPM_I2C_STS(0), buf, sizeof(buf));

	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * TPM_CR50_TIMEOUT_SHORT_MS;

	KeDelayExecutionThread(KernelMode, FALSE, &WaitInterval);
}