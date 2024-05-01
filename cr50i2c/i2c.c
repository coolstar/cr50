#include "driver.h"

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
	pDevice->InterruptServiced = false;
	WdfInterruptEnable(pDevice->Interrupt);
	return STATUS_SUCCESS;
}

static void tpm_cr50_i2c_disable_tpm_irq(PCR50_CONTEXT pDevice) {
	WdfInterruptDisable(pDevice->Interrupt);
}

static NTSTATUS tpm_cr50_i2c_read(
	_In_ PCR50_CONTEXT pDevice,
	uint8_t addr,
	uint8_t* buf,
	size_t len
) {
	NTSTATUS status = tpm_cr50_i2c_enable_tpm_irq(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = SpbWriteDataSynchronously(&pDevice->I2CContext, &addr, sizeof(uint8_t));
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

static NTSTATUS tpm_cr50_i2c_write(
	_In_ PCR50_CONTEXT pDevice,
	uint8_t addr,
	uint8_t* buf,
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
	uint8_t mask = TPM_ACCESS_VALID | TPM_ACCESS_ACTIVE_LOCALITY;
	uint8_t buf;

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

static void tpm_cr50_release_locality(PCR50_CONTEXT pDevice, bool force) {
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

static NTSTATUS tpm_cr50_request_locality(PCR50_CONTEXT pDevice) {
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
	Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Setting locality timed out 0x%x\n", status);
	return STATUS_TIMEOUT;
}

static uint8_t tpm_cr50_i2c_tis_status(PCR50_CONTEXT pDevice) {
	uint8_t buf[4];

	if (!NT_SUCCESS(tpm_cr50_i2c_read(pDevice, TPM_I2C_STS(0), buf, sizeof(buf))))
		return 0;

	return buf[0];
}

static void tpm_cr50_i2c_tis_set_ready(PCR50_CONTEXT pDevice)
{
	uint8_t buf[4] = { TPM_STS_COMMAND_READY };

	tpm_cr50_i2c_write(pDevice, TPM_I2C_STS(0), buf, sizeof(buf));

	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * TPM_CR50_TIMEOUT_SHORT_MS;

	KeDelayExecutionThread(KernelMode, false, &WaitInterval);
}


static NTSTATUS tpm_cr50_i2c_get_burst_and_status(PCR50_CONTEXT pDevice, uint8_t mask,
	size_t* burst, uint32_t* status) {
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

		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"burst/mask, status: 0x%x, mask: 0x%x, burst: %lld\n", *status & mask, mask, *burst);

		KeDelayExecutionThread(KernelMode, false, &WaitInterval);
		KeQuerySystemTimePrecise(&CurrentTime);
	}

	Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Timeout reading burst and status\n", );
	return STATUS_TIMEOUT;
}

static NTSTATUS tpm_cr50_i2c_tis_recv(PCR50_CONTEXT pDevice, uint8_t* buf, size_t buf_len) {
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
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Unexpected burstcnt: %zu (max=%zu, min=%d)\n",
			burstcnt, buf_len, TPM_HEADER_SIZE);
		ret = STATUS_IO_DEVICE_ERROR;
		goto out_err;
	}

	/* Read first chunk of burstcnt bytes */
	ret = tpm_cr50_i2c_read(pDevice, addr, buf, burstcnt);
	if (!NT_SUCCESS(ret)) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Read of first chunk failed\n");
		goto out_err;
	}

	expected = RtlUlongByteSwap(*((uint32_t*)(buf + 2)));
	if (expected > buf_len) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
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
			Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
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
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
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

static NTSTATUS tpm_cr50_i2c_tis_send(PCR50_CONTEXT pDevice, uint8_t* buf, size_t len) {
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
			Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
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
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Data still expected\n");
		ret = IO_ERROR_IO_HARDWARE_ERROR;
		goto out_err;
	}

	/* Start the TPM command */
	ret = tpm_cr50_i2c_write(pDevice, TPM_I2C_STS(0), tpm_go,
		sizeof(tpm_go));
	if (!NT_SUCCESS(ret)) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
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
static bool tpm_cr50_i2c_req_canceled(PCR50_CONTEXT pDevice, uint8_t status)
{
	UNREFERENCED_PARAMETER(pDevice);
	return status == TPM_STS_COMMAND_READY;
}