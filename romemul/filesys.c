/**
 * File: filesys.c
 * Author: Diego Parrilla Santamaría
 * Date: Novemeber 2023
 * Copyright: 2023 - GOODDATA LABS SL
 * Description: C file that contains the functions to convert from a file format to another
 * The original code comes from Hatari emulator:
 *      https://github.com/hatari/hatari/blob/69ce09390824a29c9708ae41290d6df28a5f766e/src/msa.c
 *
 * Please repect the original license
 */

#include "include/filesys.h"

/*
    .MSA FILE FORMAT
  --================------------------------------------------------------------

  For those interested, an MSA file is made up as follows:

  Header:

  Word  ID marker, should be $0E0F
  Word  Sectors per track
  Word  Sides (0 or 1; add 1 to this to get correct number of sides)
  Word  Starting track (0-based)
  Word  Ending track (0-based)

  Individual tracks follow the header in alternating side order, e.g. a double
  sided disk is stored as:

  TRACK 0, SIDE 0
  TRACK 0, SIDE 1
  TRACK 1, SIDE 0
  TRACK 1, SIDE 1
  TRACK 2, SIDE 0
  TRACK 2, SIDE 1

  ...and so on. Track blocks are made up as follows:

  Word  Data length
  Bytes  Data

  If the data length is equal to 512 x the sectors per track value, it is an
  uncompressed track and you can merely copy the data to the appropriate track
  of the disk. However, if the data length value is less than 512 x the sectors
  per track value it is a compressed track.

  Compressed tracks use simple a Run Length Encoding (RLE) compression method.
  You can directly copy any data bytes until you find an $E5 byte. This signals
  a compressed run, and is made up as follows:

  Byte  Marker - $E5
  Byte  Data byte
  Word  Run length

  So, if MSA found six $AA bytes in a row it would encode it as:

  $E5AA0006

  What happens if there's an actual $E5 byte on the disk? Well, logically
  enough, it is encoded as:

  $E5E50001

  This is obviously bad news if a disk consists of lots of data like
  $E500E500E500E500... but if MSA makes a track bigger when attempting to
  compress it, it just stores the uncompressed version instead.

  MSA only compresses runs of at least 4 identical bytes (after all, it would be
  wasteful to store 4 bytes for a run of only 3 identical bytes!). There is one
  exception to this rule: if a run of 2 or 3 $E5 bytes is found, that is stored
  appropriately enough as a run. Again, it would be wasteful to store 4 bytes
  for every single $E5 byte.

  The hacked release of MSA that enables the user to turn off compression
  completely simply stops MSA from trying this compression and produces MSA
  images that are completely uncompressed. This is okay because it is possible
  for MSA to produce such an image anyway, and such images are therefore 100%
  compatible with normal MSA versions (and MSA-to-ST of course).
*/

// Function to check if there is enough free disk space to create a file of size 'nDiskSize'
FRESULT checkDiskSpace(const char *folder, uint32_t nDiskSize)
{
    DWORD fre_clust, fre_sect, tot_sect;
    FATFS *fs;
    FRESULT fr;

    // Get free space
    fr = f_getfree(folder, &fre_clust, &fs);
    if (fr != FR_OK)
    {
        return fr; // Return error code if operation is not successful
    }

    // Calculate the total number of free bytes
    uint64_t freeBytes = fre_clust * fs->csize * NUM_BYTES_PER_SECTOR;

    // Check if there is enough space
    if ((uint64_t)nDiskSize > freeBytes)
    {
        return FR_DENIED; // Not enough space
    }
    return FR_OK; // Enough space available
}

/**
 * @brief Converts an MSA disk image file to an ST disk image file.
 *
 * This function takes a given MSA disk image file,
 * represented by `msaFilename` located within the `folder` directory, and
 * converts it into an ST (Atari ST disk image) file specified by `stFilename`.
 * If the `overwrite` is set to true, any existing file with the same
 * name as `stFilename` will be overwritten.
 *
 * @param folder The directory where the MSA file is located and the ST file will be saved.
 * @param msaFilename The name of the MSA file to convert.
 * @param stFilename The name of the ST file to be created.
 * @param overwrite If true, any existing ST file will be overwritten.
 *
 * @return FRESULT A FatFS result code indicating the status of the operation.
 */
FRESULT MSA_to_ST(const char *folder, char *msaFilename, char *stFilename, bool overwrite)
{
    MSAHEADERSTRUCT msaHeader;
    uint32_t nBytesLeft = 0;
    uint8_t *pMSAImageBuffer, *pImageBuffer;
    uint8_t Byte, Data;
    uint16_t Track, Side, DataLength, NumBytesUnCompressed, RunLength;
    uint8_t *pBuffer = NULL;
    FRESULT fr;   // FatFS function common result code
    FIL src_file; // File objects
    FIL dest_file;
    UINT br, bw; // File read/write count
    BYTE *buffer_in = NULL;
    BYTE *buffer_out = NULL;

    // Check if the folder exists, if not, exit
    DPRINTF("Checking folder %s\n", folder);
    if (f_stat(folder, NULL) != FR_OK)
    {
        DPRINTF("Folder %s not found!\n", folder);
        return FR_NO_PATH;
    }

    char src_path[256];
    char dest_path[256];

    // Create full paths for source and destination files
    sprintf(src_path, "%s/%s", folder, msaFilename);
    sprintf(dest_path, "%s/%s", folder, stFilename);
    DPRINTF("SRC PATH: %s\n", src_path);
    DPRINTF("DEST PATH: %s\n", dest_path);

    // Check if the destination file already exists
    fr = f_stat(dest_path, NULL);
    if (fr == FR_OK && !overwrite)
    {
        DPRINTF("Destination file exists and overwrite is false, canceling operation\n");
        return FR_FILE_EXISTS; // Destination file exists and overwrite is false, cancel the operation
    }

    // Check if the MSA source file exists in the SD card with FatFS
    if (f_open(&src_file, src_path, FA_READ) != FR_OK)
    {
        DPRINTF("MSA file not found!\n");
        return FR_NO_FILE;
    }
    // Calculate the size of the MSA file
    nBytesLeft = f_size(&src_file);

    // Check if the ST destination file exists in the SD card with FatFS
    if (f_open(&dest_file, dest_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        DPRINTF("Error creating destination ST file!\n");
        return FR_NO_FILE;
    }

    buffer_in = malloc(sizeof(MSAHEADERSTRUCT) * +sizeof(uint16_t));
    // Read only the memory needed to read the header AND the first track info (a word)
    fr = f_read(&src_file, buffer_in, sizeof(MSAHEADERSTRUCT) + sizeof(uint16_t), &br); // Read a chunk of source file
    if (fr != FR_OK)
    {
        DPRINTF("Error reading source file!\n");
        if (buffer_in != NULL)
        {
            free(buffer_in);
        }
        return FR_DISK_ERR;
    }

    memcpy(&msaHeader, buffer_in, sizeof(MSAHEADERSTRUCT));
    /* First swap 'header' words around to PC format - easier later on */
    msaHeader.ID = bswap_16(msaHeader.ID);
    msaHeader.SectorsPerTrack = bswap_16(msaHeader.SectorsPerTrack);
    msaHeader.Sides = bswap_16(msaHeader.Sides);
    msaHeader.StartingTrack = bswap_16(msaHeader.StartingTrack);
    msaHeader.EndingTrack = bswap_16(msaHeader.EndingTrack);
    DPRINTF("MSA Header: ID: %x\n", msaHeader.ID);
    DPRINTF("MSA Header: SectorsPerTrack: %d\n", msaHeader.SectorsPerTrack);
    DPRINTF("MSA Header: Sides: %d\n", msaHeader.Sides);
    DPRINTF("MSA Header: StartingTrack: %d\n", msaHeader.StartingTrack);
    DPRINTF("MSA Header: EndingTrack: %d\n", msaHeader.EndingTrack);

    if (msaHeader.ID != 0x0E0F || msaHeader.EndingTrack > 86 || msaHeader.StartingTrack > msaHeader.EndingTrack || msaHeader.SectorsPerTrack > 56 || msaHeader.Sides > 1 || nBytesLeft <= (long)sizeof(MSAHEADERSTRUCT))
    {
        DPRINTF("MSA image has a bad header!\n");
        if (buffer_in != NULL)
        {
            free(buffer_in);
        }
        return FR_DISK_ERR;
    }

    if (checkDiskSpace(folder, NUM_BYTES_PER_SECTOR * msaHeader.SectorsPerTrack * (msaHeader.Sides + 1) * (msaHeader.EndingTrack - msaHeader.StartingTrack)) != FR_OK)
    {
        DPRINTF("Not enough space in the SD card!\n");
        if (buffer_in != NULL)
        {
            free(buffer_in);
        }
        return FR_DENIED;
    }

    nBytesLeft -= sizeof(MSAHEADERSTRUCT);
    // The length of the first track to read
    uint16_t currentTrackDataLength = bswap_16((uint16_t) * (uint16_t *)(buffer_in + sizeof(MSAHEADERSTRUCT)));

    /* Uncompress to memory as '.ST' disk image - NOTE: assumes NUM_BYTES_PER_SECTOR bytes
     * per sector (use NUM_BYTES_PER_SECTOR define)!!! */
    for (Track = msaHeader.StartingTrack; Track <= msaHeader.EndingTrack; Track++)
    {
        for (Side = 0; Side < (msaHeader.Sides + 1); Side++)
        {
            uint16_t nBytesPerTrack = NUM_BYTES_PER_SECTOR * msaHeader.SectorsPerTrack;
            nBytesLeft -= sizeof(uint16_t);
            DPRINTF("Track: %d\n", Track);
            DPRINTF("Side: %d\n", Side);
            DPRINTF("Current Track Size: %d\n", currentTrackDataLength);
            DPRINTF("Bytes per track: %d\n", nBytesPerTrack);
            DPRINTF("Bytes left: %d\n", nBytesLeft);

            if (nBytesLeft < 0)
                goto out;

            // Reserve write buffer
            if (buffer_out != NULL)
            {
                free(buffer_out);
            }
            buffer_out = malloc(nBytesPerTrack);

            if (buffer_in != NULL)
            {
                free(buffer_in);
            }
            buffer_in = malloc(currentTrackDataLength + sizeof(uint16_t));

            BYTE *buffer_in_tmp = buffer_in;
            fr = f_read(&src_file, buffer_in_tmp, currentTrackDataLength + sizeof(uint16_t), &br); // Read a chunk of source file
            if (fr != FR_OK)
            {
                DPRINTF("Error reading source file!\n");
                if (buffer_in != NULL)
                {
                    free(buffer_in);
                }
                if (buffer_out != NULL)
                {
                    free(buffer_out);
                }
                return FR_DISK_ERR;
            }

            // Check if it is not a compressed track
            if (currentTrackDataLength == nBytesPerTrack)
            {
                nBytesLeft -= currentTrackDataLength;
                if (nBytesLeft < 0)
                    goto out;

                // No compression, read the full track and write it to the destination file
                fr = f_write(&dest_file, buffer_in, nBytesPerTrack, &bw); // Write it to the destination file
                if (fr != FR_OK)
                {
                    DPRINTF("Error writing destination file!\n");
                    if (buffer_in != NULL)
                    {
                        free(buffer_in);
                    }
                    if (buffer_out != NULL)
                    {
                        free(buffer_out);
                    }
                    return FR_DISK_ERR;
                }
                buffer_in_tmp += currentTrackDataLength;
            }
            else
            {
                // Compressed track, uncompress it
                NumBytesUnCompressed = 0;
                BYTE *buffer_out_tmp = buffer_out;
                while (NumBytesUnCompressed < nBytesPerTrack)
                {
                    if (--nBytesLeft < 0)
                        goto out;
                    Byte = *buffer_in_tmp++;
                    if (Byte != 0xE5) /* Compressed header? */
                    {
                        *buffer_out_tmp++ = Byte; /* No, just copy byte */
                        NumBytesUnCompressed++;
                    }
                    else
                    {
                        nBytesLeft -= 3;
                        if (nBytesLeft < 0)
                            goto out;
                        Data = *buffer_in_tmp++; /* Byte to copy */
                        RunLength = (uint16_t)(buffer_in_tmp[1] | buffer_in_tmp[0] << 8);
                        /* Limit length to size of track, incorrect images may overflow */
                        if (RunLength + NumBytesUnCompressed > nBytesPerTrack)
                        {
                            DPRINTF("MSA_UnCompress: Illegal run length -> corrupted disk image?\n");
                            RunLength = nBytesPerTrack - NumBytesUnCompressed;
                        }
                        buffer_in_tmp += sizeof(uint16_t);
                        for (uint16_t i = 0; i < RunLength; i++)
                        {
                            *buffer_out_tmp++ = Data; /* Copy byte */
                        }
                        NumBytesUnCompressed += RunLength;
                    }
                }
                // No compression, read the full track and write it to the destination file
                fr = f_write(&dest_file, buffer_out, nBytesPerTrack, &bw); // Write it to the destination file
                if (fr != FR_OK)
                {
                    DPRINTF("Error writing destination file!\n");
                    if (buffer_in != NULL)
                    {
                        free(buffer_in);
                    }
                    if (buffer_out != NULL)
                    {
                        free(buffer_out);
                    }
                    return FR_DISK_ERR;
                }
            }
            if (nBytesLeft > 0)
            {
                currentTrackDataLength = (uint16_t)(buffer_in_tmp[1] | buffer_in_tmp[0] << 8);
            }
        }
    }
out:
    if (nBytesLeft < 0)
    {
        DPRINTF("MSA error: Premature end of file!\n");
    }

    // Close files
    f_close(&src_file);
    f_close(&dest_file);

    if (buffer_in != NULL)
    {
        free(buffer_in);
    }
    if (buffer_out != NULL)
    {
        free(buffer_out);
    }

    return FR_OK;
}

/**
 * Write a short integer to a given address in little endian byte order.
 * This function is primarily used to write 16-bit values into the boot sector of a
 * disk image which requires values to be in little endian format.
 *
 * @param addr Pointer to the address where the short integer should be written.
 * @param val The 16-bit value to be written in little endian byte order.
 */
static inline void write_short_le(void *addr, uint16_t val)
{
    /* Cast the address to a uint8_t pointer and write the value in little endian byte order. */
    uint8_t *p = (uint8_t *)addr;

    p[0] = (uint8_t)val;        // Write the low byte.
    p[1] = (uint8_t)(val >> 8); // Write the high byte shifted down.
}

/**
 * Create .ST image according to 'Tracks,Sector,Sides' and save
 *
            40 track SS   40 track DS   80 track SS   80 track DS
    0- 1   Branch instruction to boot program if executable
    2- 7   'Loader'
    8-10   24-bit serial number
    11-12   BPS    512           512           512           512
    13      SPC     1             2             2             2
    14-15   RES     1             1             1             1
    16      FAT     2             2             2             2
    17-18   DIR     64           112           112           112
    19-20   SEC    360           720           720          1440
    21      MEDIA  $FC           $FD           $F8           $F9  (isn't used by ST-BIOS)
    22-23   SPF     2             2             5             5
    24-25   SPT     9             9             9             9
    26-27   SIDE    1             2             1             2
    28-29   HID     0             0             0             0
    510-511 CHECKSUM
 */

/**
 * Create a blank Atari ST disk image file.
 *
 * This function creates a blank Atari ST formatted disk image with the
 * specified parameters. It can also set the volume label and allows
 * for the option to overwrite an existing file.
 *
 * @param folder The directory in which to create the disk image.
 * @param stFilename The name of the disk image file to create.
 * @param nTracks Number of tracks on the disk.
 * @param nSectors Number of sectors per track.
 * @param nSides Number of disk sides.
 * @param volLabel Optional volume label for the disk; pass NULL for no label.
 * @param overwrite If true, an existing file with the same name will be overwritten.
 *
 * @return FR_OK if the operation is successful, otherwise an error code.
 */
FRESULT create_blank_ST_image(const char *folder, char *stFilename, int nTracks, int nSectors, int nSides, const char *volLavel, bool overwrite)
{
    uint8_t *pDiskHeader;
    uint32_t nDiskSize;
    uint32_t nHeaderSize;
    uint32_t nDiskSizeNoHeader;
    uint16_t SPC, nDir, MediaByte, SPF;
    uint16_t drive;
    uint16_t LabelSize;
    uint8_t *pDirStart;

    FRESULT fr; // FatFS function common result code
    FIL dest_file;
    UINT bw; // File write count
    char dest_path[256];
    BYTE zeroBuff[NUM_BYTES_PER_SECTOR]; // Temporary buffer to hold zeros

    /* Calculate size of disk image */
    nDiskSize = nTracks * nSectors * nSides * NUM_BYTES_PER_SECTOR;

    // Calculate size of the header information
    nHeaderSize = 2 * (1 + SPF_MAX) * NUM_BYTES_PER_SECTOR;

    // Calculate the size of the disk without the header
    nDiskSizeNoHeader = nDiskSize - nHeaderSize;

    // Check if the folder exists, if not, exit
    DPRINTF("Checking folder %s\n", folder);
    if (f_stat(folder, NULL) != FR_OK)
    {
        DPRINTF("Folder %s not found!\n", folder);
        return FR_NO_PATH;
    }

    if (checkDiskSpace(folder, nDiskSize) != FR_OK)
    {
        DPRINTF("Not enough space in the SD card!\n");
        return FR_DENIED;
    }

    // Create the full path for the destination file
    sprintf(dest_path, "%s/%s", folder, stFilename);
    DPRINTF("DEST PATH: %s\n", dest_path);

    // Check if the destination file already exists
    fr = f_stat(dest_path, NULL);
    if (fr == FR_OK && !overwrite)
    {
        DPRINTF("Destination file exists and overwrite is false, canceling operation\n");
        return FR_FILE_EXISTS; // Destination file exists and overwrite is false, cancel the operation
    }

    /* HD/ED disks are all double sided */
    if (nSectors >= 18)
        nSides = 2;

    // Allocate space ONLY for the header. We don't have enough space in the RP2040
    pDiskHeader = malloc(nHeaderSize);
    if (pDiskHeader == NULL)
    {
        DPRINTF("Error while creating blank disk image");
        return FR_DISK_ERR;
    }
    memset(pDiskHeader, 0, nHeaderSize); /* Clear buffer */

    /* Fill in boot-sector */
    pDiskHeader[0] = 0xE9;            /* Needed for MS-DOS compatibility */
    memset(pDiskHeader + 2, 0x4e, 6); /* 2-7 'Loader' */

    write_short_le(pDiskHeader + 8, rand()); /* 8-10 24-bit serial number */
    pDiskHeader[10] = rand();

    write_short_le(pDiskHeader + 11, NUM_BYTES_PER_SECTOR); /* 11-12 BPS */

    if ((nTracks == 40) && (nSides == 1))
        SPC = 1;
    else
        SPC = 2;
    pDiskHeader[13] = SPC; /* 13 SPC */

    write_short_le(pDiskHeader + 14, 1); /* 14-15 RES */
    pDiskHeader[16] = 2;                 /* 16 FAT */

    if (SPC == 1)
        nDir = 64;
    else if (nSectors < 18)
        nDir = 112;
    else
        nDir = 224;
    write_short_le(pDiskHeader + 17, nDir); /* 17-18 DIR */

    write_short_le(pDiskHeader + 19, nTracks * nSectors * nSides); /* 19-20 SEC */

    if (nSectors >= 18)
        MediaByte = 0xF0;
    else
    {
        if (nTracks <= 42)
            MediaByte = 0xFC;
        else
            MediaByte = 0xF8;
        if (nSides == 2)
            MediaByte |= 0x01;
    }
    pDiskHeader[21] = MediaByte; /* 21 MEDIA */

    if (nSectors >= 18)
        SPF = SPF_MAX;
    else if (nTracks >= 80)
        SPF = 5;
    else
        SPF = 2;
    write_short_le(pDiskHeader + 22, SPF); /* 22-23 SPF */

    write_short_le(pDiskHeader + 24, nSectors); /* 24-25 SPT */
    write_short_le(pDiskHeader + 26, nSides);   /* 26-27 SIDE */
    write_short_le(pDiskHeader + 28, 0);        /* 28-29 HID */

    /* Set correct media bytes in the 1st FAT: */
    pDiskHeader[NUM_BYTES_PER_SECTOR] = MediaByte;
    pDiskHeader[NUM_BYTES_PER_SECTOR + 1] = pDiskHeader[NUM_BYTES_PER_SECTOR + 2] = 0xFF;
    /* Set correct media bytes in the 2nd FAT: */
    pDiskHeader[NUM_BYTES_PER_SECTOR + SPF * NUM_BYTES_PER_SECTOR] = MediaByte;
    pDiskHeader[(NUM_BYTES_PER_SECTOR + 1) + SPF * NUM_BYTES_PER_SECTOR] =
        pDiskHeader[(2 + NUM_BYTES_PER_SECTOR) + SPF * NUM_BYTES_PER_SECTOR] = 0xFF;

    /* Set volume label if needed (in 1st entry of the directory) */
    if (volLavel != NULL)
    {
        /* Set 1st dir entry as 'volume label' */
        pDirStart = pDiskHeader + (1 + SPF * 2) * NUM_BYTES_PER_SECTOR;
        memset(pDirStart, ' ', 8 + 3);
        LabelSize = strlen(volLavel);
        if (LabelSize <= 8 + 3)
            memcpy(pDirStart, volLavel, LabelSize);
        else
            memcpy(pDirStart, volLavel, 8 + 3);

        pDirStart[8 + 3] = GEMDOS_FILE_ATTRIB_VOLUME_LABEL;
    }

    // Always create a new file. We assume we have already checked if the file exists and the overwrite
    if (f_open(&dest_file, dest_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    {
        DPRINTF("Error creating the destination ST file!\n");
        free(pDiskHeader);
        return FR_NO_FILE;
    }

    // Write the header to the destination file
    fr = f_write(&dest_file, pDiskHeader, nHeaderSize, &bw); // Write it to the destination file
    if (fr != FR_OK)
    {
        DPRINTF("Error writing the header to the destination ST file!\n");
        free(pDiskHeader);
        return FR_DISK_ERR;
    }

    // Write zeros to the rest of the file

    memset(zeroBuff, 0, sizeof(zeroBuff)); // Set the buffer to zeros
    while (nDiskSizeNoHeader > 0)
    {
        UINT toWrite = sizeof(zeroBuff);
        if (nDiskSizeNoHeader < toWrite)
            toWrite = nDiskSizeNoHeader; // Write only as much as needed

        fr = f_write(&dest_file, zeroBuff, toWrite, &bw); // Write zeros to file
        if (fr != FR_OK || bw < toWrite)
        {
            fr = (fr == FR_OK) ? FR_DISK_ERR : fr; // If no error during write, set the error to disk error
            free(pDiskHeader);
            return fr;
        }
        nDiskSizeNoHeader -= bw; // Decrement the remaining size
    }

    // Close the file
    f_close(&dest_file);

    // Free buffer
    free(pDiskHeader);
    return FR_OK;
}

/**
 * @brief Copies a file within the same folder to a new file with a specified name.
 *
 * This function reads a specified file from a specified folder, and creates a new file
 * with a specified name in the same folder, copying the contents of the original file
 * to the new file. If a file with the specified new name already exists, the behavior
 * depends on the value of the overwrite_flag argument: if true, the existing file is
 * overwritten; if false, the function returns an error code and the operation is canceled.
 *
 * @param folder The path of the folder containing the source file, as a null-terminated string.
 * @param src_filename The name of the source file, as a null-terminated string.
 * @param dest_filename The name for the new file, as a null-terminated string.
 * @param overwrite_flag A flag indicating whether to overwrite the destination file
 *        if it already exists: true to overwrite, false to cancel the operation.
 *
 * @return A result code of type FRESULT, indicating the result of the operation:
 *         - FR_OK on success.
 *         - FR_FILE_EXISTS if the destination file exists and overwrite_flag is false.
 *         - Other FatFS error codes for other error conditions.
 *
 * @note This function uses the FatFS library to perform file operations, and is designed
 *       to work in environments where FatFS is available. It requires the ff.h header file.
 *
 * Usage:
 * @code
 * FRESULT result = copy_file("/folder", "source.txt", "destination.txt", true);  // Overwrite if destination.txt exists
 * @endcode
 */
FRESULT copy_file(const char *folder, const char *src_filename, const char *dest_filename, bool overwrite_flag)
{
    FRESULT fr;   // FatFS function common result code
    FIL src_file; // File objects
    FIL dest_file;
    UINT br, bw;       // File read/write count
    BYTE buffer[4096]; // File copy buffer

    char src_path[256];
    char dest_path[256];

    // Create full paths for source and destination files
    sprintf(src_path, "%s/%s", folder, src_filename);
    sprintf(dest_path, "%s/%s", folder, dest_filename);

    DPRINTF("Copying file '%s' to '%s'. Overwrite? %s\n", src_path, dest_path, overwrite_flag ? "YES" : "NO");

    // Check if the destination file already exists
    FILINFO fno;
    fr = f_stat(dest_path, &fno);
    if (fr == FR_OK && !overwrite_flag)
    {
        DPRINTF("Destination file exists and overwrite_flag is false, canceling operation\n");
        return FR_FILE_EXISTS; // Destination file exists and overwrite_flag is false, cancel the operation
    }

    // Open the source file
    fr = f_open(&src_file, src_path, FA_READ);
    if (fr != FR_OK)
    {
        DPRINTF("f_open error: %s (%d)\n", FRESULT_str(fr), fr);
        return fr;
    }

    // Create and open the destination file
    fr = f_open(&dest_file, dest_path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        DPRINTF("f_open error: %s (%d)\n", FRESULT_str(fr), fr);
        f_close(&src_file); // Close the source file if it was opened successfully
        return fr;
    }

    // Copy the file
    do
    {
        fr = f_read(&src_file, buffer, sizeof buffer, &br); // Read a chunk of source file
        if (fr != FR_OK)
            break;                                 // Break on error
        fr = f_write(&dest_file, buffer, br, &bw); // Write it to the destination file
    } while (fr == FR_OK && br == sizeof buffer);

    // Close files
    f_close(&src_file);
    f_close(&dest_file);

    DPRINTF("File copied\n");
    return fr; // Return the result
}

/**
 * @brief Checks if a directory exists in the filesystem.
 *
 * This function uses the FatFS library's f_stat() method to determine if
 * a specified directory exists on the filesystem. It returns 1 if the directory
 * exists and 0 otherwise.
 *
 * @param dir A pointer to a string containing the path of the directory to check.
 * @return int Returns 1 if the directory exists, 0 otherwise.
 */
int directory_exists(const char *dir)
{
    FILINFO fno;
    FRESULT res = f_stat(dir, &fno);

    // Check if the result is OK and if the attribute indicates it's a directory
    if (res == FR_OK && (fno.fattrib & AM_DIR))
    {
        return 1; // Directory exists
    }

    return 0; // Directory does not exist
}

/**
 * @brief Retrieves the total and free storage space of an SD card.
 *
 * This function queries the SD card's filesystem to obtain the total and free storage space.
 * It calculates the size in megabytes and updates the respective variables pointed by
 * `totalSize_MB` and `freeSpace_MB`.
 *
 * @param fs_ptr Pointer to the filesystem object associated with the SD card.
 * @param totalSize_MB Pointer to a variable where the total size of the SD card will be stored (in megabytes).
 * @param freeSpace_MB Pointer to a variable where the free space of the SD card will be stored (in megabytes).
 *
 * @note If the function fails to retrieve the free space information, the values pointed to by
 * `totalSize_MB` and `freeSpace_MB` are set to zero. This function assumes that `fs_ptr` is a valid
 * pointer to an initialized `FATFS` structure and that `totalSize_MB` and `freeSpace_MB` are valid
 * non-null pointers.
 *
 * Usage:
 *     FATFS fs;
 *     uint32_t totalSize, freeSpace;
 *     get_card_info(&fs, &totalSize, &freeSpace);
 */
void get_card_info(FATFS *fs_ptr, uint32_t *totalSize_MB, uint32_t *freeSpace_MB)
{
    DWORD fre_clust;

    // Set initial values to zero as a precaution
    *totalSize_MB = 0;
    *freeSpace_MB = 0;

    // Get volume information and free clusters of drive
    if (f_getfree("", &fre_clust, &fs_ptr) != FR_OK)
    {
        return; // Error handling: Set values to zero if getfree fails
    }

    // Calculate total sectors in the SD card
    uint64_t total_sectors = (fs_ptr->n_fatent - 2) * fs_ptr->csize;

    // Convert total sectors to bytes and then to megabytes
    *totalSize_MB = (total_sectors * NUM_BYTES_PER_SECTOR) / 1048576;

    // Convert free clusters to sectors and then to bytes
    uint64_t free_space_bytes = (uint64_t)fre_clust * fs_ptr->csize * NUM_BYTES_PER_SECTOR;

    // Convert bytes to megabytes
    *freeSpace_MB = free_space_bytes / 1048576;
}

FRESULT read_and_trim_file(const char *path, char **content, size_t max_length)
{
    FIL fil;              // File object
    FRESULT fr;           // FatFs return code
    UINT br;              // Read count
    char tempBuffer[512]; // Temporary buffer for reading file

    DPRINTF("Reading file: %s\n", path);
    // Check if the file exists
    fr = f_stat(path, NULL);
    if (fr != FR_OK)
    {
        DPRINTF("File does not exist or another error occur: %s\n", path);
        return fr; // File does not exist or another error occurred
    }

    DPRINTF("File exists: %s. Opening.\n", path);
    // Open the file
    fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK)
    {
        DPRINTF("Error opening file: %s\n", path);
        return fr; // Error opening file
    }

    long length = f_size(&fil);
    DPRINTF("File size: %ld\n", length);

    *content = malloc(length + 1);

    // Allocate or reallocate memory for content
    if (*content == NULL)
    {
        DPRINTF("Unable to allocate memory\n");
        f_close(&fil);
        return FR_INT_ERR; // Allocation error
    }

    DPRINTF("Reading file content\n");
    **content = '\0'; // Set first character to null

    // Read file content
    while (f_read(&fil, tempBuffer, sizeof(tempBuffer) - 1, &br) == FR_OK && br > 0)
    {
        tempBuffer[br] = '\0'; // Null-terminate the read string
        // Process and trim the buffer content here...
        // For simplicity, this code only trims spaces, CR, and LF from the start and end.
        char *start = tempBuffer;
        char *end = start + strlen(tempBuffer) - 1;

        // Trim leading spaces, CR, and LF
        while (*start && (isspace((unsigned char)*start) || *start == '\r' || *start == '\n'))
            start++;

        // Trim trailing spaces, CR, and LF
        while (end > start && (isspace((unsigned char)*end) || *end == '\r' || *end == '\n'))
            end--;
        *(end + 1) = '\0'; // Set new null terminator

        // Append trimmed content to *content
        size_t currentLength = strlen(*content);
        size_t newLength = currentLength + strlen(start) + 1;
        *content = realloc(*content, newLength);
        if (*content == NULL)
        {
            f_close(&fil);
            DPRINTF("Error allocating memory\n");
            return FR_INT_ERR; // Allocation error
        }
        strcat(*content, start);
    }

    // Close the file
    f_close(&fil);
    DPRINTF("File content: '%s'\n", *content);

    // Trim content to the maximum length
    if (strlen(*content) > max_length)
    {
        (*content)[max_length] = '\0';
    }
    return FR_OK;
}
/**
 * @brief Splits a full path into its drive letter, folder path, and file pattern components.
 *
 * This function takes a full path that includes a drive letter, a path of folders, and a file pattern,
 * and splits it into three separate components. The drive letter is identified by the colon following it,
 * the folder path is the part of the path before the file pattern, and the file pattern is the last segment
 * of the path. The function handles various cases, including paths without a drive letter or folder path.
 *
 * @param fullPath A string representing the full path to split. It should include the drive letter, folder path,
 *                 and file pattern. Example: "C:\\Users\\Public\\Documents\\*.txt".
 * @param drive A pointer to a character array where the extracted drive letter will be stored.
 * @param folders A pointer to a character array where the extracted folder path will be stored.
 * @param filePattern A pointer to a character array where the extracted file pattern will be stored.
 *
 * Example usage:
 *     char drive[10], folders[256], filePattern[100];
 *     split_fullpath("C:\\Users\\Public\\Documents\\*.txt", drive, folders, filePattern);
 *     // `drive`, `folders`, and `filePattern` now contain the respective components of the path.
 */
void split_fullpath(const char *fullPath, char *drive, char *folders, char *filePattern)
{
    const char *driveEnd;
    const char *pathEnd;

    // Initialize the output strings
    drive[0] = '\0';
    folders[0] = '\0';
    filePattern[0] = '\0';

    // Find the position of the first ':' to identify the drive
    driveEnd = strchr(fullPath, ':');
    if (driveEnd != NULL)
    {
        strncpy(drive, fullPath, driveEnd - fullPath + 1);
        drive[driveEnd - fullPath + 1] = '\0'; // Null-terminate the drive string
        fullPath = driveEnd + 1;               // Adjust fullPath to point after the drive letter
    }

    // Find the last '\\' or '/' to separate folders and file pattern
    char slash = fullPath[strcspn(fullPath, "\\/")] == '\\' ? '\\' : '/';
    pathEnd = strrchr(fullPath, slash);
    if (pathEnd != NULL)
    {
        strncpy(folders, fullPath, pathEnd - fullPath + 1);
        folders[pathEnd - fullPath + 1] = '\0'; // Null-terminate the folders string
        strcpy(filePattern, pathEnd + 1);       // Copy the file pattern
    }
    else
    {
        // If there's no '\\' or '/', then the entire path is considered the file pattern
        strcpy(filePattern, fullPath);
    }
}

/**
 * @brief Converts all backslash characters to forward slashes in a given string.
 *
 * This function iterates through the characters of the provided string and replaces
 * each backslash ('\\') character with a forward slash ('/'). This is typically used
 * to convert file paths from Windows-style to Unix-style. The function operates in place,
 * modifying the original string. It is safe to use with strings that do not contain
 * backslashes, as the function will simply leave them unchanged.
 *
 * @param path A pointer to a character array (string) that will be modified in place.
 *             The array should be null-terminated.
 *
 * Example usage:
 *     char path[] = "C:\\Users\\Public\\Documents\\file.txt";
 *     back_2_forwardslash(path);
 *     // `path` is now "C:/Users/Public/Documents/file.txt"
 */
void back_2_forwardslash(char *path)
{
    if (path == NULL)
        return;

    for (int i = 0; path[i] != '\0'; i++)
    {
        if (path[i] == '\\')
        {
            path[i] = '/';
        }
    }
}

/**
 * @brief Converts all forward slash characters to backslashes in a given string.
 *
 * This function iterates through the characters of the provided string and replaces
 * each forward slash ('/') character with a backslash ('\\'). This is typically used
 * to convert file paths from Unix-style to Windows-style. The function operates in place,
 * modifying the original string. It is safe to use with strings that do not contain
 * forward slashes, as the function will simply leave them unchanged.
 *
 * @param path A pointer to a character array (string) that will be modified in place.
 *             The array should be null-terminated.
 *
 * Example usage:
 *     char path[] = "C:/Users/Public/Documents/file.txt";
 *     forward_2_backslash(path);
 *     // `path` is now "C:\\Users\\Public\\Documents\\file.txt"
 */
void forward_2_backslash(char *path)
{
    if (path == NULL)
        return;

    for (int i = 0; path[i] != '\0'; i++)
    {
        if (path[i] == '/')
        {
            path[i] = '\\';
        }
    }
}

static void sanitize_dos_name(char *name)
{
    const char *invalid_chars = "<>:\"/\\|?*";
    for (size_t i = 0; name[i]; i++)
    {
        if (name[i] <= 31 || strchr(invalid_chars, name[i]))
        {
            name[i] = '_';
        }
    }
}

/**
 * @brief Shortens a long file name to a DOS 8.3 filename format in uppercase and stores it in a provided array.
 *
 * This function takes a long file name, shortens it to comply with the DOS 8.3 format, and stores it in uppercase
 * in the provided array. For filenames with multiple dots, it uses the last dot as the extension separator,
 * replaces other dots with underscores in the name part, and formats the name accordingly.
 *
 * @param originalName A pointer to a constant character array containing the original long file name.
 *                     The name should include the extension and must be null-terminated.
 * @param shortenedName A pointer to a character array of size 13 where the shortened file name will be stored.
 *                      This array will be modified by the function to contain the new file name.
 *
 * Example usage:
 *     char shortenedFileName[13];
 *     shorten_fname("file.with.many.dots.ext", shortenedFileName);
 *     printf("Shortened File Name: %s\n", shortenedFileName); // Output: FILE_W~1.EXT
 */
void shorten_fname(const char *originalName, char shortenedName[13])
{
    char namePart[9];   // 8 characters for name + null terminator
    char extPart[4];    // 3 characters for extension + null terminator
    char tempName[256]; // Temporary buffer to process the name part

    // Initialize the arrays
    memset(namePart, 0, sizeof(namePart));
    memset(extPart, 0, sizeof(extPart));
    memset(tempName, 0, sizeof(tempName));
    memset(shortenedName, 0, 13);

    // Find the last dot in the filename to separate name and extension
    const char *dot = strrchr(originalName, '.');
    size_t nameLength;
    size_t originalNameLength = strlen(originalName);

    if (dot && dot != originalName)
    {
        // Calculate the length of the name part
        nameLength = dot - originalName;

        // Copy the extension part (exclude the dot)
        size_t extLength = strlen(dot + 1);
        if (extLength > 3)
            extLength = 3; // Truncate extension to 3 characters

        strncpy(extPart, dot + 1, extLength);
        extPart[extLength] = '\0';
    }
    else
    {
        // No extension found
        nameLength = originalNameLength;
    }

    // Copy the name part into tempName
    if (nameLength >= sizeof(tempName))
        nameLength = sizeof(tempName) - 1; // Prevent overflow
    strncpy(tempName, originalName, nameLength);
    tempName[nameLength] = '\0';

    // Replace any dots in tempName with underscores
    for (size_t i = 0; tempName[i]; i++)
    {
        if (tempName[i] == '.')
            tempName[i] = '_';
    }

    // Sanitize namePart and extPart by replacing invalid characters
    void sanitize_dos_name(char *name)
    {
        const char *invalid_chars = "<>:\"/\\|?*";
        for (size_t i = 0; name[i]; i++)
        {
            if (name[i] <= 31 || strchr(invalid_chars, name[i]))
            {
                name[i] = '_';
            }
        }
    }

    sanitize_dos_name(tempName);
    sanitize_dos_name(extPart);

    // Now process tempName to fit into namePart
    size_t tempNameLength = strlen(tempName);

    if (tempNameLength > 8)
    {
        // Use the first 6 characters, then '~1' to make 8 characters
        strncpy(namePart, tempName, 6);
        namePart[6] = '~';
        namePart[7] = '1'; // In a real implementation, this should be incremented to avoid collisions
        namePart[8] = '\0';
    }
    else
    {
        // Copy the name part as is
        strncpy(namePart, tempName, 8);
        namePart[8] = '\0';
    }

    // Convert name and extension to uppercase
    for (size_t i = 0; namePart[i]; i++)
        namePart[i] = toupper((unsigned char)namePart[i]);

    for (size_t i = 0; extPart[i]; i++)
        extPart[i] = toupper((unsigned char)extPart[i]);

    // Format the shortened filename
    if (extPart[0] != '\0')
    {
        // Include the extension
        snprintf(shortenedName, 13, "%s.%s", namePart, extPart);
    }
    else
    {
        // No extension
        strncpy(shortenedName, namePart, 13);
        shortenedName[12] = '\0'; // Ensure null-termination
    }
}

/**
 * @brief Removes consecutive duplicate slashes from a string.
 *
 * This function iterates through the provided string, `str`, and when it
 * finds two consecutive forward slashes, it removes one of them. This
 * process is done in-place, modifying the original string.
 *
 * @param str The string from which duplicate slashes are to be removed.
 *            This parameter is modified in place.
 *
 * @note The function modifies the string in place. Ensure the provided
 *       string is modifiable and not a string literal.
 *
 * Example Usage:
 *     char path[] = "path//to//your//directory/";
 *     remove_dup_slashes(path);
 *     // path is now "path/to/your/directory/"
 */
void remove_dup_slashes(char *str)
{
    for (char *p = str; *p != '\0'; p++)
    {
        if (*p == '/' && *(p + 1) == '/')
        {
            memmove(p, p + 1, strlen(p));
            p--; // Adjust pointer to recheck this position after removal
        }
    }
}

/**
 * @brief Converts FATFS attributes to ST attributes.
 *
 * This function takes FATFS attributes as input and converts them to ST attributes.
 * The conversion is done by checking each bit of the FAT attributes and setting
 * the corresponding bit in the ST attributes if it is set in the FATFS attributes.
 *
 * @param fat_attribs The FATFS attributes to be converted.
 * @return The converted ST attributes.
 */
uint8_t attribs_fat2st(uint8_t fat_attribs)
{
    uint8_t st_attribs = 0;

    if (fat_attribs & AM_RDO)
    {
        st_attribs |= FS_ST_READONLY;
    }
    if (fat_attribs & AM_HID)
    {
        st_attribs |= FS_ST_HIDDEN;
    }
    if (fat_attribs & AM_SYS)
    {
        st_attribs |= FS_ST_SYSTEM;
    }
    if (fat_attribs & AM_DIR)
    {
        st_attribs |= FS_ST_FOLDER;
    }
    if (fat_attribs & AM_ARC)
    {
        st_attribs |= FS_ST_ARCH;
    }
    return st_attribs;
}

/**
 * @brief Converts ST attributes to FATFS attributes.
 *
 * This function takes ST attributes as input and converts them to FATFS attributes.
 * The conversion is done by checking each bit of the ST attributes and setting
 * the corresponding bit in the FATFS attributes if it is set in the ST attributes.
 *
 * @param st_attribs The ST attributes to be converted.
 * @return The converted FATFS attributes.
 */
uint8_t attribs_st2fat(uint8_t st_attribs)

{
    uint8_t fat_attribs = 0;

    if (st_attribs & FS_ST_READONLY)
    {
        fat_attribs |= AM_RDO;
    }
    if (st_attribs & FS_ST_HIDDEN)
    {
        fat_attribs |= AM_HID;
    }
    if (st_attribs & FS_ST_SYSTEM)
    {
        fat_attribs |= AM_SYS;
    }
    if (st_attribs & FS_ST_FOLDER)
    {
        fat_attribs |= AM_DIR;
    }
    if (st_attribs & FS_ST_ARCH)
    {
        fat_attribs |= AM_ARC;
    }
    return fat_attribs;
}

/**
 * @brief Converts ST attributes to a string representation.
 *
 * This function takes ST attributes as input and converts them to a string representation.
 * The conversion is done by checking each bit of the ST attributes and setting
 * the corresponding character in the string if it is set in the ST attributes.
 *
 * @param attribs_str The string to store the representation. It should be at least 6 characters long.
 * @param st_attribs The ST attributes to be converted.
 */
void get_attribs_st_str(char *attribs_str, uint8_t st_attribs)
{
    strcpy(attribs_str, "------");
    if (st_attribs & FS_ST_READONLY)
    {
        attribs_str[0] = 'R';
    }
    if (st_attribs & FS_ST_HIDDEN)
    {
        attribs_str[1] = 'H';
    }
    if (st_attribs & FS_ST_SYSTEM)
    {
        attribs_str[2] = 'S';
    }
    if (st_attribs & FS_ST_LABEL)
    {
        attribs_str[3] = 'L';
    }
    if (st_attribs & FS_ST_FOLDER)
    {
        attribs_str[4] = 'D';
    }
    if (st_attribs & FS_ST_ARCH)
    {
        attribs_str[5] = 'A';
    }
    return;
}

/**
 * @brief Converts a filename to uppercase.
 *
 * This function takes a filename as input and converts all its characters to uppercase.
 * The converted filename is stored in a separate string. The original filename remains unchanged.
 * The function ensures that the converted filename is null-terminated.
 *
 * @param originalName The original filename to be converted. It should be a null-terminated string.
 * @param upperName The string to store the converted filename. It should be at least 14 characters long.
 */
void upper_fname(const char *originalName, char upperName[14])
{
    int i = 0;
    while (originalName[i] != '\0' && i < 13)
    {
        upperName[i] = toupper(originalName[i]);
        i++;
    }
    upperName[i] = '\0'; // Null terminate the string
}

/**
 * @brief Filters a filename.
 *
 * This function takes a filename as input and filters it by removing non-alphanumeric characters.
 * The filtered filename is stored in a separate string. The original filename remains unchanged.
 * The function ensures that the filtered filename is null-terminated.
 *
 * @param originalName The original filename to be filtered. It should be a null-terminated string.
 * @param filteredName The string to store the filtered filename. It should be at least 14 characters long.
 */
void filter_fname(const char *originalName, char filteredName[14])

{
    int i = 0, j = 0;
    while (originalName[i] != '\0' && j < 13)
    {
        // Check for alphanumeric characters
        if (isalnum((unsigned char)originalName[i]))
        {
            filteredName[j++] = originalName[i];
        }
        else
        {
            // Check for specific symbols
            // Ugly as HELL, but not using regular expressions
            switch (originalName[i])
            {
            case '_':
            case '!':
            case '@':
            case '#':
            case '$':
            case '%':
            case '^':
            case '&':
            case '(':
            case ')':
            case '+':
            case '-':
            case '=':
            case '~':
            case '`':
            case ';':
            case '\'':
            case ',':
            case '.':
            case '<':
            case '>':
            case '|':
            case '[':
            case ']':
            case '{':
            case '}':
                filteredName[j++] = originalName[i];
                break;
            default:
                // Ignore any other characters
                break;
            }
        }
        i++;
    }
    filteredName[j] = '\0'; // Null-terminate the filtered name
}

/**
 * @brief Extract the filename from a full path
 *
 * Takes a full path or url and extracts the filename from it at the right end until a slash or
 * backslash is found. The extracted filename is stored in a separate string. The original path
 *
 * @param url The full path or url from which to extract the filename. It should be a null-terminated string.
 * @param filename The string to store the extracted filename. It should be at least 256 characters long.
 */
void extract_filename(const char *url, char filename[256])
{
    int i = strlen(url) - 1;
    while (i >= 0 && url[i] != '/' && url[i] != '\\')
    {
        i--;
    }
    strcpy(filename, url + i + 1);
}

/**
 * @brief Check if the filename is a read-write floppy disk image file with the ".rw" extension.
 *
 * This file contains functions related to file system operations in the Atari ST sidecart
 * emulation project. It provides a utility function to check if a given filename is a
 * read-write floppy disk image file.
 *
 * @param filename The name of the file to check.
 * @return True if the filename ends with ".rw", indicating a read-write floppy disk image file.
 *         False otherwise.
 */
bool is_floppy_rw(const char *filename)
{
    return (strlen(filename) >= 3 && strcmp(filename + strlen(filename) - 3, ".rw") == 0);
}

/**
 * @brief Change the SPI speed for the SD card.
 *
 * This function changes the SPI speed for the SD card based on the configured baud rate.
 * It retrieves the baud rate from the configuration entry and updates the SPI baud rate accordingly.
 * If the baud rate is invalid or not found, it uses the default value.
 */
void change_spi_speed()
{
    size_t sd_num = sd_get_num();
    if (sd_num > 0)
    {
        ConfigEntry *sd_baud_rate_kb = find_entry(PARAM_SD_BAUD_RATE_KB);
        if (sd_baud_rate_kb != NULL)
        {
            int baud_rate = atoi(sd_baud_rate_kb->value);
            if (baud_rate > 0)
            {
                DPRINTF("Changing SD card baud rate to %i\n", baud_rate);
                sd_card_t *sd_card = sd_get_by_num(sd_num - 1);
                sd_card->spi_if_p->spi->baud_rate = baud_rate * 1000;
            }
            else
            {
                DPRINTF("Invalid baud rate. Using default value\n");
            }
        }
    }
    else
    {
        DPRINTF("SD card not found\n");
    }
}