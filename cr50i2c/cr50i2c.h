#ifndef __CR50_I2C_REGS_H__
#define __CR50_I2C_REGS_H__

enum tis_access {
	TPM_ACCESS_VALID = 0x80,
	TPM_ACCESS_ACTIVE_LOCALITY = 0x20,
	TPM_ACCESS_REQUEST_PENDING = 0x04,
	TPM_ACCESS_REQUEST_USE = 0x02,
	TPM_ACCESS_ESTABLISHMENT = 0x01
};

enum tis_status {
	TPM_STS_VALID = 0x80,
	TPM_STS_COMMAND_READY = 0x40,
	TPM_STS_GO = 0x20,
	TPM_STS_DATA_AVAIL = 0x10,
	TPM_STS_DATA_EXPECT = 0x08
};

enum tis_defaults {
	TIS_SHORT_TIMEOUT = 750,
	TIS_LONG_TIMEOUT = 2000,
	TIS_MEM_BASE = 0xFED40000,
	TIS_MEM_LEN = 0x5000
};

#define 	TPM_WARN_DOING_SELFTEST   0x802
#define 	TPM_ERR_DEACTIVATED   0x6
#define 	TPM_ERR_DISABLED   0x7
#define 	TPM_HEADER_SIZE   10
#define 	TPM_DIGEST_SIZE   20
#define 	TPM_MAX_RNG_DATA   128

#define TPM_CR50_MAX_BUFSIZE		64
#define TPM_CR50_TIMEOUT_SHORT_MS	2		/* Short timeout during transactions */
#define TPM_CR50_TIMEOUT_NOIRQ_MS	20		/* Timeout for TPM ready without IRQ */
#define TPM_CR50_DID_VID		0x00281ae0L	/* Device and vendor ID reg value */
#define TPM_TI50_DID_VID		0x504a6666L	/* Device and vendor ID reg value */
#define TPM_CR50_I2C_MAX_RETRIES	3		/* Max retries due to I2C errors */
#define TPM_CR50_I2C_RETRY_DELAY_LO	55		/* Min usecs between retries on I2C */
#define TPM_CR50_I2C_RETRY_DELAY_HI	65		/* Max usecs between retries on I2C */

#define TPM_I2C_ACCESS(l)	(0x0000 | ((l) << 4))
#define TPM_I2C_STS(l)		(0x0001 | ((l) << 4))
#define TPM_I2C_DATA_FIFO(l)	(0x0005 | ((l) << 4))
#define TPM_I2C_DID_VID(l)	(0x0006 | ((l) << 4))

#define TPM_LOCALITY_0_SPI_BASE 0x00

/* Assorted TPM2 registers for interface type FIFO. */
#define	TPM_ACCESS(l)			(0x0000 | ((l) << 12))
#define	TPM_INT_ENABLE(l)		(0x0008 | ((l) << 12))
#define	TPM_INT_VECTOR(l)		(0x000C | ((l) << 12))
#define	TPM_INT_STATUS(l)		(0x0010 | ((l) << 12))
#define	TPM_INTF_CAPS(l)		(0x0014 | ((l) << 12))
#define	TPM_STS(l)			(0x0018 | ((l) << 12))
#define	TPM_STS3(l)			(0x001b | ((l) << 12))
#define	TPM_DATA_FIFO(l)		(0x0024 | ((l) << 12))

#define	TPM_DID_VID(l)			(0x0F00 | ((l) << 12))
#define	TPM_RID(l)			(0x0F04 | ((l) << 12))

#define TPM_FW_VER_SPI	  (TPM_LOCALITY_0_SPI_BASE + 0xf90)
#define CR50_BOARD_CFG_SPI     (TPM_LOCALITY_0_SPI_BASE + 0xfe0)

#define CR50_TIMEOUT_INIT_MS_SPI 30000 /* Very long timeout for TPM init */

#endif /* __CR50_I2C_REGS_H__ */