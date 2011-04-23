#ifdef __CYGWIN__
#ifndef IRPDECODE_H
#define IRPDECODE_H

// Decodes an IRP (and associated IO stack) to locate the current stack entry
// and the IRP major number.
//
// Returns non-negative on success.
int fusent_decode_irp(IRP *irp, IO_STACK_LOCATION *iosp, uint8_t *outirptype,
		IO_STACK_LOCATION **outiosp);

#endif /* IRPDECODE_H */
#endif /* __CYGWIN__ */
