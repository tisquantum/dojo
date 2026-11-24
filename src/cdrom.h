/* The port's virtual CD drive, as the rest of the port sees it. */

#ifndef PORT_CDROM_H
#define PORT_CDROM_H

/* Reads one 2352-byte sector of the disc image; returns 0 past its end. */
int CdReadRawSector(int lba, unsigned char *out);
/* Runs the drive for one field: delivers data sectors and advances XA streams. */
void CdVerticalBlank(void);
/* An XA stream's sector under the head, for the audio decoder (xa.c). */
void CdXaSector(const unsigned char *raw);

#endif
