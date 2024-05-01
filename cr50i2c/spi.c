#include "driver.h"

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

static NTSTATUS tpm2_write_reg_spi(
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

static NTSTATUS tpm2_read_reg_spi(
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

static void tpm_cr50_release_locality(PCR50_CONTEXT pDevice, BOOLEAN force) {
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

static NTSTATUS tpm_cr50_request_locality(PCR50_CONTEXT pDevice) {
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

static UINT8 tpm_cr50_spi_tis_status(PCR50_CONTEXT pDevice) {
	UINT8 buf[4];

	if (!NT_SUCCESS(tpm2_read_reg_spi(pDevice, TPM_STS(0), buf, sizeof(buf))))
		return 0;

	return buf[0];
}

static void tpm_cr50_spi_tis_set_ready(PCR50_CONTEXT pDevice)
{
	UINT8 buf[4] = { TPM_STS_COMMAND_READY };

	tpm2_write_reg_spi(pDevice, TPM_STS(0), buf, sizeof(buf));

	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * TPM_CR50_TIMEOUT_SHORT_MS;

	KeDelayExecutionThread(KernelMode, FALSE, &WaitInterval);
}


static NTSTATUS tpm_cr50_spi_get_burst_and_status(PCR50_CONTEXT pDevice, UINT8 mask,
	size_t* burst, UINT32* status) {
	LARGE_INTEGER StopTime;

	LARGE_INTEGER CurrentTime;
	KeQuerySystemTimePrecise(&CurrentTime);

	UINT8 buf[4];
	*status = 0;

	StopTime.QuadPart = CurrentTime.QuadPart + (10 * 1000 * TIS_LONG_TIMEOUT);
	while (CurrentTime.QuadPart < StopTime.QuadPart) {
		NTSTATUS ret = tpm2_read_reg_spi(pDevice, TPM_STS(0), buf, sizeof(buf));
		LARGE_INTEGER WaitInterval;
		WaitInterval.QuadPart = -10 * 1000 * TPM_CR50_TIMEOUT_SHORT_MS;

		if (!NT_SUCCESS(ret)) {
			KeDelayExecutionThread(KernelMode, FALSE, &WaitInterval);

			KeQuerySystemTimePrecise(&CurrentTime);
			continue;
		}

		*status = *buf;
		*burst = *((UINT16*)(buf + 1));

		if ((*status & mask) == mask && *burst > 0 && *burst <= TPM_CR50_MAX_BUFSIZE - 1)
			return STATUS_SUCCESS;

		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"burst/mask, status: 0x%x, mask: 0x%x, burst: %lld\n", *status & mask, mask, *burst);

		KeDelayExecutionThread(KernelMode, FALSE, &WaitInterval);
		KeQuerySystemTimePrecise(&CurrentTime);
	}

	Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
		"Timeout reading burst and status\n", );
	return STATUS_TIMEOUT;
}

static NTSTATUS tpm_cr50_spi_tis_recv(PCR50_CONTEXT pDevice, UINT8* buf, size_t buf_len) {
	UINT8 mask = TPM_STS_VALID | TPM_STS_DATA_AVAIL;
	size_t burstcnt, cur, len, expected;
	UINT8 addr = TPM_DATA_FIFO(0);
	UINT32 status;
	NTSTATUS ret;

	if (buf_len < TPM_HEADER_SIZE) {
		return STATUS_INVALID_BUFFER_SIZE;
	}

	ret = tpm_cr50_spi_get_burst_and_status(pDevice, mask, &burstcnt, &status);
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
	ret = tpm2_read_reg_spi(pDevice, addr, buf, burstcnt);
	if (!NT_SUCCESS(ret)) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Read of first chunk failed\n");
		goto out_err;
	}

	expected = RtlUlongByteSwap(*((UINT32*)(buf + 2)));
	if (expected > buf_len) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Buffer too small to receive i2c data\n");
		ret = STATUS_BUFFER_TOO_SMALL;
		goto out_err;
	}

	/* Now read the rest of the data */
	cur = burstcnt;
	while (cur < expected) {
		ret = tpm_cr50_spi_get_burst_and_status(pDevice, mask, &burstcnt, &status);
		if (!NT_SUCCESS(ret)) {
			goto out_err;
		}

		len = min((size_t)(burstcnt), (size_t)(expected - cur));
		ret = tpm2_read_reg_spi(pDevice, addr, buf + cur, len);
		if (!NT_SUCCESS(ret)) {
			Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Read failed\n");
			goto out_err;
		}

		cur += len;
	}

	/* Ensure TPM is done reading data */
	ret = tpm_cr50_spi_get_burst_and_status(pDevice, TPM_STS_VALID, &burstcnt, &status);
	if (!NT_SUCCESS(ret)) {
		goto out_err;
	}

	if (status & TPM_STS_DATA_AVAIL) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Data still available\n");
		ret = IO_ERROR_IO_HARDWARE_ERROR;
		goto out_err;
	}

	tpm_cr50_release_locality(pDevice, FALSE);
	return ret;

out_err:
	if (tpm_cr50_spi_tis_status(pDevice) & TPM_STS_COMMAND_READY)
		tpm_cr50_spi_tis_set_ready(pDevice);

	tpm_cr50_release_locality(pDevice, FALSE);
	return ret;
}

static NTSTATUS tpm_cr50_spi_tis_send(PCR50_CONTEXT pDevice, UINT8* buf, size_t len) {
	size_t burstcnt, limit, sent = 0;
	UINT8 tpm_go[4] = { TPM_STS_GO };
	UINT32 status;
	NTSTATUS ret;

	ret = tpm_cr50_request_locality(pDevice);
	if (!NT_SUCCESS(ret))
		return ret;

	/* Wait until TPM is ready for a command */
	LARGE_INTEGER StopTime;

	LARGE_INTEGER CurrentTime;
	KeQuerySystemTimePrecise(&CurrentTime);
	StopTime.QuadPart = CurrentTime.QuadPart + (10 * 1000 * TIS_LONG_TIMEOUT);

	while (!(tpm_cr50_spi_tis_status(pDevice) & TPM_STS_COMMAND_READY)) {
		KeQuerySystemTimePrecise(&CurrentTime);
		if (CurrentTime.QuadPart > StopTime.QuadPart) {
			ret = STATUS_TIMEOUT;
			goto out_err;
		}

		tpm_cr50_spi_tis_set_ready(pDevice);
	}

	while (len > 0) {
		UINT8 mask = TPM_STS_VALID;

		/* Wait for data if this is not the first chunk */
		if (sent > 0)
			mask |= TPM_STS_DATA_EXPECT;

		/* Read burst count and check status */
		ret = tpm_cr50_spi_get_burst_and_status(pDevice, mask, &burstcnt, &status);
		if (!NT_SUCCESS(ret))
			goto out_err;

		/*
		 * Use burstcnt - 1 to account for the address byte
		 * that is inserted by tpm_cr50_i2c_write()
		 */
		limit = min((size_t)(burstcnt - 1), (size_t)(len));
		ret = tpm2_write_reg_spi(pDevice, TPM_DATA_FIFO(0), &buf[sent], limit);
		if (!NT_SUCCESS(ret)) {
			Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Write failed\n");
			goto out_err;
		}

		sent += limit;
		len -= limit;
	}

	/* Ensure TPM is not expecting more data */
	ret = tpm_cr50_spi_get_burst_and_status(pDevice, TPM_STS_VALID, &burstcnt, &status);
	if (!NT_SUCCESS(ret))
		goto out_err;
	if (status & TPM_STS_DATA_EXPECT) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Data still expected\n");
		ret = IO_ERROR_IO_HARDWARE_ERROR;
		goto out_err;
	}

	/* Start the TPM command */
	ret = tpm2_write_reg_spi(pDevice, TPM_STS(0), tpm_go,
		sizeof(tpm_go));
	if (!NT_SUCCESS(ret)) {
		Cr50Print(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Start command failed\n");
		goto out_err;
	}
	return STATUS_SUCCESS;

out_err:
	/* Abort current transaction if still pending */
	if (tpm_cr50_spi_tis_status(pDevice) & TPM_STS_COMMAND_READY)
		tpm_cr50_spi_tis_set_ready(pDevice);

	tpm_cr50_release_locality(pDevice, FALSE);
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
static BOOLEAN tpm_cr50_spi_req_canceled(PCR50_CONTEXT pDevice, UINT8 status)
{
	UNREFERENCED_PARAMETER(pDevice);
	return status == TPM_STS_COMMAND_READY;
}