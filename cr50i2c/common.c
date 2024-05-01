#include "driver.h"

//I2C functions
NTSTATUS tpm_cr50_i2c_read(
	_In_ PCR50_CONTEXT pDevice,
	UINT8 addr,
	UINT8* buf,
	size_t len
);
NTSTATUS tpm_cr50_i2c_write(
	_In_ PCR50_CONTEXT pDevice,
	UINT8 addr,
	UINT8* buf,
	size_t len
);

void tpm_cr50_i2c_release_locality(PCR50_CONTEXT pDevice, BOOLEAN force);
NTSTATUS tpm_cr50_i2c_request_locality(PCR50_CONTEXT pDevice);
NTSTATUS tpm_cr50_i2c_tis_status(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz);
NTSTATUS tpm_cr50_i2c_tis_status_write(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz);
void tpm_cr50_i2c_tis_set_ready(PCR50_CONTEXT pDevice);

//SPI functions
NTSTATUS tpm2_write_reg_spi(
	_In_  PCR50_CONTEXT  pDevice,
	_In_  UINT32 regNumber,
	_In_  UINT8* buffer,
	_In_  size_t bytes
);
NTSTATUS tpm2_read_reg_spi(
	_In_  PCR50_CONTEXT  pDevice,
	_In_  UINT32 regNumber,
	_Out_  UINT8* buffer,
	_In_  size_t bytes
);

void tpm_cr50_spi_release_locality(PCR50_CONTEXT pDevice, BOOLEAN force);
NTSTATUS tpm_cr50_spi_request_locality(PCR50_CONTEXT pDevice);
NTSTATUS tpm_cr50_spi_tis_status(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz);
NTSTATUS tpm_cr50_spi_tis_status_write(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz);
void tpm_cr50_spi_tis_set_ready(PCR50_CONTEXT pDevice);

void tpm_cr50_release_locality(PCR50_CONTEXT pDevice, BOOLEAN force) {
	if (pDevice->Transport == CR50_TRANSPORT_I2C) {
		tpm_cr50_i2c_release_locality(pDevice, force);
		return;
	}
	else if (pDevice->Transport == CR50_TRANSPORT_SPI) {
		tpm_cr50_spi_release_locality(pDevice, force);
		return;
	}
	ASSERTMSG("Invalid Transport", FALSE);
}

NTSTATUS tpm_cr50_request_locality(PCR50_CONTEXT pDevice) {
	if (pDevice->Transport == CR50_TRANSPORT_I2C) {
		return tpm_cr50_i2c_request_locality(pDevice);
	}
	else if (pDevice->Transport == CR50_TRANSPORT_SPI) {
		return tpm_cr50_spi_request_locality(pDevice);
	}
	ASSERTMSG("Invalid Transport", FALSE);
}

NTSTATUS tpm_cr50_tis_status(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz) {
	if (pDevice->Transport == CR50_TRANSPORT_I2C) {
		return tpm_cr50_i2c_tis_status(pDevice, buf, sz);
	}
	else if (pDevice->Transport == CR50_TRANSPORT_SPI) {
		return tpm_cr50_spi_tis_status(pDevice, buf, sz);
	}
	ASSERTMSG("Invalid Transport", FALSE);
}

NTSTATUS tpm_cr50_tis_status_write(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz) {
	if (pDevice->Transport == CR50_TRANSPORT_I2C) {
		return tpm_cr50_i2c_tis_status_write(pDevice, buf, sz);
	}
	else if (pDevice->Transport == CR50_TRANSPORT_SPI) {
		return tpm_cr50_spi_tis_status_write(pDevice, buf, sz);
	}
	ASSERTMSG("Invalid Transport", FALSE);
}

void tpm_cr50_tis_set_ready(PCR50_CONTEXT pDevice) {
	if (pDevice->Transport == CR50_TRANSPORT_I2C) {
		tpm_cr50_i2c_tis_set_ready(pDevice);
		return;
	}
	else if (pDevice->Transport == CR50_TRANSPORT_SPI) {
		tpm_cr50_spi_tis_set_ready(pDevice);
		return;
	}
	ASSERTMSG("Invalid Transport", FALSE);
}

NTSTATUS tpm_cr50_tis_read_data_fifo(PCR50_CONTEXT pDevice, UINT8* buf, size_t burstcnt) {
	if (pDevice->Transport == CR50_TRANSPORT_I2C) {
		return tpm_cr50_i2c_read(pDevice, TPM_I2C_DATA_FIFO(0), buf, burstcnt);
	}
	else if (pDevice->Transport == CR50_TRANSPORT_SPI) {
		return tpm2_read_reg_spi(pDevice, TPM_DATA_FIFO(0), buf, burstcnt);
	}
	ASSERTMSG("Invalid Transport", FALSE);
}

NTSTATUS tpm_cr50_tis_write_data_fifo(PCR50_CONTEXT pDevice, UINT8* buf, size_t burstcnt) {
	if (pDevice->Transport == CR50_TRANSPORT_I2C) {
		return tpm_cr50_i2c_write(pDevice, TPM_I2C_DATA_FIFO(0), buf, burstcnt);
	}
	else if (pDevice->Transport == CR50_TRANSPORT_SPI) {
		return tpm2_write_reg_spi(pDevice, TPM_DATA_FIFO(0), buf, burstcnt);
	}
	ASSERTMSG("Invalid Transport", FALSE);
}

NTSTATUS tpm_cr50_read_vendor(PCR50_CONTEXT pDevice, UINT8* buf, size_t sz) {
	if (pDevice->Transport == CR50_TRANSPORT_I2C) {
		return tpm_cr50_i2c_read(pDevice, TPM_I2C_DID_VID(0), buf, sz);
	}
	else if (pDevice->Transport == CR50_TRANSPORT_SPI) {
		return tpm2_read_reg_spi(pDevice, TPM_DID_VID(0), buf, sz);
	}
	ASSERTMSG("Invalid Transport", FALSE);
}