/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Error codes: functions return 0 (or a count) on success and a negative
 * code from this list on failure, with the values Linux uses. The host
 * tests may already have them from <errno.h>.
 */
#ifndef DSPBOOT_ERRCODES_H
#define DSPBOOT_ERRCODES_H

#ifndef ENOENT
#define ENOENT		2	/* nothing there (empty slot, no file sent) */
#define EIO		5	/* hardware did not do what it should */
#define ENOEXEC		8	/* not an image dspboot can start */
#define ENODEV		19	/* device missing or not responding */
#define EINVAL		22	/* bad argument */
#define EFBIG		27	/* too large for the destination */
#define EROFS		30	/* write protected */
#define ERANGE		34	/* address or size out of range */
#define ENOMSG		42	/* no ECC bytes in the spare area */
#define ENODATA		61	/* erased page */
#define EPROTO		71	/* protocol error */
#define EBADMSG		74	/* checksum or ECC failure */
#define EOPNOTSUPP	95	/* recognised, but not supported */
#define ETIMEDOUT	110
#define ECANCELED	125
#endif

/* Text for a negative code */
const char *errstr(int err);

#endif
