/**
 * File: gemdrvemul.c
 * Author: Diego Parrilla Santamaría
 * Date: November 2023
 * Copyright: 2023 - GOODDATA LABS SL
 * Description: Emulate GEMDOS hard disk driver
 */

#include "include/gemdrvemul.h"
#include "include/sidetnfs_probe.h"
#include "include/sidetnfs_config_drive_backend.h"

// Let's substitute the flags
static uint16_t active_command_id = 0xFFFF;

static uint16_t *payloadPtr = NULL;
static uint32_t random_token;

static char *fullpath_a = NULL;
static char *hd_folder = NULL;
// Fase 8C: explicit "SD card is currently mounted and usable" status, set
// only alongside a genuinely successful f_mount() (both in the TNFS
// backend's best-effort diagnostic mount attempt and the SD backend's own
// real mount -- see GEMDRVEMUL_PING). Distinct from hd_folder != NULL only
// in that it's a clearly-named, explicitly-documented status bit rather
// than an implicit pointer-nullness convention -- see the hd_folder_ready
// comment below for why that distinction matters here.
static bool sd_available = false;
static bool debug = false;
static char drive_letter = 'C';

// Save Dgetdrv variables
static uint16_t dgetdrive_value = 0xFFFF;

// Save Fcreate variables
static bool fcreate_call = false;
static uint16_t fcreate_mode = 0xFFFF;

// Save Dsetpath variables
static char dpath_string[MAX_FOLDER_LENGTH] = {0};

// Save Fsetdta variables
static DTANode *dtaTbl[DTA_HASH_TABLE_SIZE];

// Structures to store the file descriptors
static FileDescriptors *fdescriptors = NULL; // Initialize the head of the list to NULL
static PD *pexec_pd = NULL;
static ExecHeader *pexec_exec_header = NULL;

// Y2K patch
static bool y2k_patch_enabled = false;

// XBIOS vector
static uint32_t xbios_trap_address_old = 0;
static bool xbios_reentry_locked = false;

static inline void __not_in_flash_func(generate_random_token_seed)(const TransmissionProtocol *protocol)
{
    random_token = ((*((uint32_t *)protocol->payload) & 0xFFFF0000) >> 16) | ((*((uint32_t *)protocol->payload) & 0x0000FFFF) << 16);
}

static void __not_in_flash_func(write_random_token)(uint32_t memory_shared_address)
{
    *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RANDOM_TOKEN)) = random_token;
}

// Erase the values in the DTA transfer area
static void __not_in_flash_func(nullify_dta)(uint32_t memory_shared_address)
{
    // for (uint8_t i = 0; i < DTA_SIZE_ON_ST; i += 1)
    // {
    //     uint32_t mem_acc = memory_shared_address + GEMDRVEMUL_DTA_TRANSFER + i;
    //     DPRINTF("Setting memory address %x to 0xEE\n", mem_acc);
    //     *((volatile uint8_t *)(mem_acc)) = 0xEE;
    // }
    memset((void *)(memory_shared_address + GEMDRVEMUL_DTA_TRANSFER), 0, DTA_SIZE_ON_ST);
}

// Hash function
static unsigned int __not_in_flash_func(hash)(uint32_t key)
{
    return key % DTA_HASH_TABLE_SIZE;
}

// Insert function
static void __not_in_flash_func(insertDTA)(uint32_t key, DTA data, DIR *dj, FILINFO *fno, uint32_t attribs)
{
    unsigned int index = hash(key);
    DTANode *newNode = malloc(sizeof(DTANode));
    if (newNode == NULL)
    {
        // Handle allocation error
        return;
    }

    newNode->key = key;
    newNode->data = data;
    newNode->dj = dj;
    newNode->fno = fno;
    newNode->attribs = attribs;
    // To copy the content of pat from dj to newNode:
    if (dj->pat != NULL)
    {
        newNode->pat = malloc(strlen(dj->pat) + 1); // +1 for the null terminator
        if (newNode->pat != NULL)
        {
            strcpy(newNode->pat, dj->pat);
        }
        else
        {
            DPRINTF("Memory allocation failed for newNode->pat\n");
        }
    }
    else
    {
        newNode->pat = NULL;
    }
    newNode->next = NULL;

    if (dtaTbl[index] == NULL)
    {
        dtaTbl[index] = newNode;
    }
    else
    {
        // Handle collision with separate chaining
        newNode->next = dtaTbl[index];
        dtaTbl[index] = newNode;
    }
}

// Lookup function
static DTANode *__not_in_flash_func(lookupDTA)(uint32_t key)
{
    unsigned int index = hash(key);
    DTANode *current = dtaTbl[index];
    DTANode *new_current = NULL;

    while (current != NULL)
    {
        if (current->key == key)
        {
            new_current = current;
            new_current->dj->pat = current->pat;
        }
        DPRINTF("DTA key: %x, filinfo_fname: %s, dir_obj: %x, pattern: %s\n", current->key, current->fno->fname, (void *)current->dj, current->dj->pat);
        current = current->next;
    }
    if (new_current == NULL)
    {
        DPRINTF("DTA key: %x not found\n", key);
    }
    else
    {
        DPRINTF("Returning DTA key: %x\n", new_current->key);
    }
    return new_current;
}

// Release function
static void __not_in_flash_func(releaseDTA)(uint32_t key)
{
    unsigned int index = hash(key);
    DTANode *current = dtaTbl[index];
    DTANode *prev = NULL;

    while (current != NULL)
    {
        if (current->key == key)
        {
            if (prev == NULL)
            {
                // The node to be deleted is the first node in the list
                dtaTbl[index] = current->next;
            }
            else
            {
                // The node to be deleted is not the first node
                prev->next = current->next;
            }
            if (current->dj != NULL)
            {
                f_closedir(current->dj); // Close the directory
                free(current->dj);       // Free the memory of the DIR
            }
            if (current->fno != NULL)
            {
                free(current->fno); // Free the memory of the FILINFO
            }
            free(current->pat); // Free the memory of the pat
            free(current);      // Free the memory of the node
            return;
        }

        prev = current;
        current = current->next;
    }
}

// Count the number of elements in the hash table
unsigned int __not_in_flash_func(countDTA)()
{
    unsigned int totalCount = 0;
    for (int i = 0; i < DTA_HASH_TABLE_SIZE; i++)
    {
        DTANode *currentNode = dtaTbl[i];
        while (currentNode != NULL)
        {
            totalCount++;
            currentNode = currentNode->next;
        }
    }
    return totalCount;
}
// Initialize the hash table
static void __not_in_flash_func(initializeDTAHashTable)()
{
    for (int i = 0; i < DTA_HASH_TABLE_SIZE; ++i)
    {
        dtaTbl[i] = NULL;
    }
}

// Clean the hash table
static void __not_in_flash_func(cleanDTAHashTable)()
{
    for (int i = 0; i < DTA_HASH_TABLE_SIZE; ++i)
    {
        DTANode *currentNode = dtaTbl[i];
        while (currentNode != NULL)
        {
            DTANode *nextNode = currentNode->next;
            if (currentNode->dj != NULL)
            {
                f_closedir(currentNode->dj); // Close the directory
                free(currentNode->dj);       // Free the memory of the DIR
            }
            if (currentNode->fno != NULL)
            {
                free(currentNode->fno); // Free the memory of the FILINFO
            }
            free(currentNode); // Free the memory of the node
            currentNode = nextNode;
        }
        dtaTbl[i] = NULL;
    }
}

static void __not_in_flash_func(seach_path_2_st)(const char *fspec_str, char *internal_path, char *path_forwardslash, char *name_pattern)
{
    char drive[2] = {0};
    char path[MAX_FOLDER_LENGTH] = {0};
    split_fullpath(fspec_str, drive, path_forwardslash, name_pattern);

    // Concatenate again the drive with the path
    snprintf(path, sizeof(path), "%s%s", drive, path_forwardslash);

    back_2_forwardslash(path_forwardslash);

    // Concatenate the path with the hd_folder
    snprintf(internal_path, MAX_FOLDER_LENGTH * 2, "%s/%s", hd_folder, path_forwardslash);

    // Remove duplicated forward slashes
    char *p = internal_path;
    while ((p = strstr(p, "//")) != NULL)
    {
        memmove(p, p + 1, strlen(p));
    }

    // Patterns do not work with FatFs as Atari ST expects, so we need to adjust them
    // Remove the asterisk if it is the last character behind a dot
    size_t np_len = strlen(name_pattern);
    if (np_len >= 2 && name_pattern[np_len - 1] == '*' && name_pattern[np_len - 2] == '.')
    {
        name_pattern[np_len - 2] = '\0'; // Handle pattern ending with ".*"
    }

    // If the pattern starts with a slash or a backslash, remove it
    if (name_pattern[0] == '/' || name_pattern[0] == '\\')
    {
        memmove(name_pattern, name_pattern + 1, strlen(name_pattern)); // Handle leading slashes or backslashes
    }
}

static void __not_in_flash_func(remove_trailing_spaces)(char *str)
{
    int len = strlen(str);

    // Start from the end of the string and move backwards
    while (len > 0 && str[len - 1] == ' ')
    {
        len--;
    }

    // Null-terminate the string at the new length
    str[len] = '\0';
}

static void __not_in_flash_func(populate_dta)(uint32_t memory_address_dta, uint32_t dta_address, int16_t gemdos_err_code)
{
    nullify_dta(memory_address_dta);
    // Search the folder for the files
    DTANode *dataNode = lookupDTA(dta_address);
    DTA *data = dataNode != NULL ? &dataNode->data : NULL;
    if (data != NULL)
    {
        *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_F_FOUND)) = 0;
        FILINFO *fno = dataNode->fno;
        if (fno)
        {
            strcpy(data->d_name, fno->fname);
            strcpy(data->d_fname, fno->fname);
            data->d_offset_drive = 0;
            data->d_curbyt = 0;
            data->d_curcl = 0;
            data->d_attr = attribs_fat2st(fno->fattrib);
            data->d_attrib = attribs_fat2st(fno->fattrib);
            data->d_time = fno->ftime;
            data->d_date = fno->fdate;
            data->d_length = (uint32_t)fno->fsize;
            // Ignore the reserved field

            // Transfer the DTA to the Atari ST
            // Copy the DTA to the shared memory
            for (uint8_t i = 0; i < 12; i += 1)
            {
                *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + i)) = (uint8_t)data->d_name[i];
            }
            CHANGE_ENDIANESS_BLOCK16(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 30, 14);
            *((volatile uint32_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 12)) = data->d_offset_drive;
            *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 16)) = data->d_curbyt;
            *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 18)) = data->d_curcl;
            *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 20)) = data->d_attr;
            *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 21)) = data->d_attrib;
            CHANGE_ENDIANESS_BLOCK16(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 20, 2);
            *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 22)) = data->d_time;
            *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 24)) = data->d_date;
            // Assuming memory_address_dta is a byte-addressable pointer (e.g., uint8_t*)
            uint32_t value = ((data->d_length << 16) & 0xFFFF0000) | ((data->d_length >> 16) & 0xFFFF);
            uint16_t *address = (uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 26);
            address[1] = (value >> 16) & 0xFFFF; // Most significant 16 bits
            address[0] = value & 0xFFFF;         // Least significant 16 bits
            for (uint8_t i = 0; i < 14; i += 1)
            {
                *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 30 + i)) = (uint8_t)data->d_fname[i];
            }
            CHANGE_ENDIANESS_BLOCK16(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 30, 14);
            char attribs_str[7] = "";
            get_attribs_st_str(attribs_str, *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 21)));
            DPRINTF("Populate DTA. addr: %x - attrib: %s - time: %d - date: %d - length: %x - filename: %s\n",
                    dta_address,
                    attribs_str,
                    *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 22)),
                    *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 24)),
                    ((uint32_t)address[0] << 16) | address[1],
                    (char *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 30));
        }
        else
        {
            // If no more files found, return ENMFIL for Fsnext
            // If no files found, return EFILNF for Fsfirst
            DPRINTF("DTA at %x showing error code: %x\n", dta_address, gemdos_err_code);
            *((volatile int16_t *)(memory_address_dta + GEMDRVEMUL_DTA_F_FOUND)) = (int16_t)gemdos_err_code;
            // release the memory allocated for the hash table
            releaseDTA(dta_address);
            DPRINTF("DTA at %x released. DTA table elements: %d\n", dta_address, countDTA());
            if (gemdos_err_code == GEMDOS_EFILNF)
            {
                DPRINTF("Files not found in FSFIRST.\n");
            }
            else
            {
                DPRINTF("No more files found in FSNEXT.\n");
            }
        }
    }
    else
    {
        // No DTA structure found, return error
        DPRINTF("DTA not found at %x\n", dta_address);
        *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_F_FOUND)) = 0xFFFF;
    }
}

#if SIDETNFS_USE_TNFS_LISTING
// Fase 5N (experimental): fill the Atari-side DTA transfer area directly
// from a normalized TNFS entry, mirroring populate_dta()'s exact byte
// layout (including its pre-write endianness-swap of the still-zeroed
// filename area, harmless since nullify_dta() already zeroed it -- kept
// only to stay byte-for-byte consistent with the proven function above).
// Unlike populate_dta(), this never touches the FatFS DTA hash table/DIR/
// FILINFO at all -- this search's bookkeeping lives entirely in
// sidetnfs_probe.c's own single-slot experimental state.
static void __not_in_flash_func(populate_dta_from_sidetnfs_entry)(uint32_t memory_address_dta,
                                                                    const SidetnfsAtariDirEntry *entry)
{
    nullify_dta(memory_address_dta);
    *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_F_FOUND)) = 0;

    char d_name[12] = {0};
    char d_fname[14] = {0};
    strncpy(d_name, entry->name, sizeof(d_name) - 1);
    strncpy(d_fname, entry->name, sizeof(d_fname) - 1);

    for (uint8_t i = 0; i < 12; i++)
    {
        *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + i)) = (uint8_t)d_name[i];
    }
    CHANGE_ENDIANESS_BLOCK16(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 30, 14);
    *((volatile uint32_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 12)) = 0; // d_offset_drive
    *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 16)) = 0; // d_curbyt
    *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 18)) = 0; // d_curcl
    *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 20)) = entry->attr;
    *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 21)) = entry->attr;
    CHANGE_ENDIANESS_BLOCK16(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 20, 2);
    *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 22)) = entry->time;
    *((volatile uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 24)) = entry->date;

    uint32_t value = ((entry->size << 16) & 0xFFFF0000) | ((entry->size >> 16) & 0xFFFF);
    uint16_t *address = (uint16_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 26);
    address[1] = (value >> 16) & 0xFFFF;
    address[0] = value & 0xFFFF;

    for (uint8_t i = 0; i < 14; i++)
    {
        *((volatile uint8_t *)(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 30 + i)) = (uint8_t)d_fname[i];
    }
    CHANGE_ENDIANESS_BLOCK16(memory_address_dta + GEMDRVEMUL_DTA_TRANSFER + 30, 14);
}
#endif // SIDETNFS_USE_TNFS_LISTING

static void __not_in_flash_func(add_file)(FileDescriptors **head, FileDescriptors *newFDescriptor, const char *fpath, FIL fobject, uint16_t new_fd)
{
    strncpy(newFDescriptor->fpath, fpath, 127);
    newFDescriptor->fpath[127] = '\0'; // Ensure null-termination
    newFDescriptor->fobject = fobject;
    newFDescriptor->fd = new_fd;
    newFDescriptor->offset = 0;
    newFDescriptor->backend = GEMDRIVE_FILE_BACKEND_SD;
    newFDescriptor->tnfs_handle = 0;
    newFDescriptor->next = *head;
    *head = newFDescriptor;
    DPRINTF("File %s added with fd %i\n", fpath, new_fd);
}

// Fase 7D: parallel to add_file() above, for TNFS-backed opens. Never
// touches fobject (left zeroed by the memset below) -- only tnfs_handle is
// meaningful for a GEMDRIVE_FILE_BACKEND_TNFS entry.
// Fase 7K: writable records whether this handle was opened for writing
// (Fopen mode 1/2, or Fcreate) -- see FileDescriptors.tnfs_writable.
static void __not_in_flash_func(add_tnfs_file)(FileDescriptors **head, FileDescriptors *newFDescriptor, const char *fpath, uint8_t tnfs_handle, uint16_t new_fd, bool writable)
{
    memset(newFDescriptor, 0, sizeof(*newFDescriptor));
    strncpy(newFDescriptor->fpath, fpath, 127);
    newFDescriptor->fpath[127] = '\0';
    newFDescriptor->fd = new_fd;
    newFDescriptor->offset = 0;
    newFDescriptor->backend = GEMDRIVE_FILE_BACKEND_TNFS;
    newFDescriptor->tnfs_handle = tnfs_handle;
    newFDescriptor->tnfs_writable = writable;
    newFDescriptor->next = *head;
    *head = newFDescriptor;
    DPRINTF("TNFS file %s added with fd %i, tnfs handle %u, writable %d\n", fpath, new_fd, tnfs_handle, (int)writable);
}

// Fase 10B: parallel to add_file()/add_tnfs_file() above, for the
// read-only config-flash drive. Stores a direct pointer into the existing
// Fase 10A const array -- never a copy -- plus its length; offset starts
// at 0, exactly like every other backend's freshly opened file.
static void __not_in_flash_func(add_config_flash_file)(FileDescriptors **head, FileDescriptors *newFDescriptor,
                                                          const char *fpath, const uint8_t *data, uint32_t size,
                                                          uint16_t new_fd)
{
    memset(newFDescriptor, 0, sizeof(*newFDescriptor));
    strncpy(newFDescriptor->fpath, fpath, 127);
    newFDescriptor->fpath[127] = '\0';
    newFDescriptor->fd = new_fd;
    newFDescriptor->offset = 0;
    newFDescriptor->backend = GEMDRIVE_FILE_BACKEND_CONFIG_FLASH;
    newFDescriptor->config_flash_data = data;
    newFDescriptor->config_flash_size = size;
    newFDescriptor->next = *head;
    *head = newFDescriptor;
    DPRINTF("Config-flash file %s added with fd %i, size %lu\n", fpath, new_fd, (unsigned long)size);
}

static void __not_in_flash_func(print_file_descriptors)(FileDescriptors *head)
{
    FileDescriptors *current = head;
    while (current != NULL)
    {
        DPRINTF("File descriptor: %i - File path: %s\n", current->fd, current->fpath);
        current = current->next;
    }
}

static FileDescriptors *__not_in_flash_func(get_file_by_fpath)(FileDescriptors *head, const char *fpath)
{
    while (head != NULL)
    {
        if (strcmp(head->fpath, fpath) == 0)
        {
            return head;
        }
        head = head->next;
    }
    return NULL;
}

static FileDescriptors *__not_in_flash_func(get_file_by_fdesc)(FileDescriptors *head, uint16_t fd)
{
    while (head != NULL)
    {
        DPRINTF("Comparing %i with %i\n", head->fd, fd);
        if (head->fd == fd)
        {
            DPRINTF("File descriptor found. Returning %i\n", head->fd);
            return head;
        }
        head = head->next;
    }
    return NULL;
}

static void __not_in_flash_func(delete_file_by_fpath)(FileDescriptors **head, const char *fpath)
{
    FileDescriptors *current = *head;
    FileDescriptors *prev = NULL;

    while (current != NULL)
    {
        if (strcmp(current->fpath, fpath) == 0)
        {
            if (prev == NULL)
            {
                *head = current->next;
            }
            else
            {
                prev->next = current->next;
            }
            free(current);
            return;
        }
        prev = current;
        current = current->next;
    }
}

static void __not_in_flash_func(delete_file_by_fdesc)(FileDescriptors **head, uint16_t fd)
{
    FileDescriptors *current = *head;
    FileDescriptors *prev = NULL;

    while (current != NULL)
    {
        if (current->fd == fd)
        { // Adjust this line based on the actual structure of FIL
            {
                if (prev == NULL)
                {
                    *head = current->next;
                }
                else
                {
                    prev->next = current->next;
                }
                free(current);
                return;
            }
            prev = current;
            current = current->next;
        }
    }
}

// Comparator function for qsort
static uint16_t __not_in_flash_func(compare_fd)(const void *a, const void *b)
{
    return (*(uint16_t *)a - *(uint16_t *)b);
}

static uint16_t __not_in_flash_func(get_first_available_fd)(FileDescriptors *head)
{
    if (head == NULL)
    {
        DPRINTF("List is empty. Returning %i\n", FIRST_FILE_DESCRIPTOR);
        return FIRST_FILE_DESCRIPTOR; // Return if the list is empty
    }

    // Count the number of nodes
    int count = 0;
    FileDescriptors *current = head;
    while (current != NULL)
    {
        count++;
        current = current->next;
    }

    count++;
    int *fdArray = (int *)malloc((count) * sizeof(int));
    if (!fdArray)
    {
        printf("Memory allocation failed\n");
        return (uint16_t)INT_MAX;
    }

    fdArray[0] = FIRST_FILE_DESCRIPTOR; // Add the first file descriptor to the array always
    current = head;
    for (int i = 1; i < count; i++)
    {
        fdArray[i] = current->fd;
        current = current->next;
    }

    // Sort the array by fd
    qsort(fdArray, count, sizeof(uint16_t), compare_fd);

    // Find the smallest gap
    for (uint16_t i = 0; i < count - 1; i++)
    {
        if (fdArray[i + 1] - fdArray[i] > 1)
        {
            uint16_t val = fdArray[i] + 1; // Found a gap, return the first available number
            free(fdArray);                 // Free the allocated memory
            return val;
        }
    }
    uint16_t val = fdArray[count - 1] + 1; // No gap found, return the last value plus 1
    free(fdArray);                         // Free the allocated memory
    return val;
}

// payloadPtr, dpath_string, hd_folder are global variables
static void __not_in_flash_func(get_local_full_pathname)(char *tmp_filepath)
{
    // Obtain the fname string and keep it in memory
    // concatenated path and filename
    char path_filename[MAX_FOLDER_LENGTH] = {0};
    char tmp_path[MAX_FOLDER_LENGTH] = {0};

    COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, path_filename, MAX_FOLDER_LENGTH);
    DPRINTF("dpath_string: %s\n", dpath_string);
    DPRINTF("path_filename: %s\n", path_filename);
    if (path_filename[1] == ':')
    {
        // If the path has the drive letter, jump two positions
        // and ignore the dpath_string
        snprintf(path_filename, MAX_FOLDER_LENGTH, "%s", path_filename + 2);
        DPRINTF("New path_filename: %s\n", path_filename);
        snprintf(tmp_path, MAX_FOLDER_LENGTH, "%s/", hd_folder);
    }
    else if (path_filename[0] == '\\')
    {
        // If the path filename has a backslash, ignore the dpath_string
        DPRINTF("New path_filename: %s\n", path_filename);
        snprintf(tmp_path, MAX_FOLDER_LENGTH, "%s/", hd_folder);
    }
    else
    {
        // If the path filename does not have a drive letter,
        // concatenate the path with the hd_folder and the filename
        // If the path has the drive letter, jump two positions
        if (dpath_string[1] == ':')
        {
            snprintf(tmp_path, MAX_FOLDER_LENGTH, "%s/%s", hd_folder, dpath_string + 2);
        }
        else
        {
            snprintf(tmp_path, MAX_FOLDER_LENGTH, "%s/%s", hd_folder, dpath_string);
        }
    }
    snprintf(tmp_filepath, MAX_FOLDER_LENGTH, "%s/%s", tmp_path, path_filename);
    back_2_forwardslash(tmp_filepath);

    // Remove duplicated forward slashes
    remove_dup_slashes(tmp_filepath);
    DPRINTF("tmp_filepath: %s\n", tmp_filepath);
}

// Fase 7D: TNFS-relative counterpart of get_local_full_pathname() above --
// same drive-letter/backslash/dpath_string mapping logic (reads the same
// payloadPtr/dpath_string globals), but never prefixes hd_folder. Produces
// the same forward-slash, GEMDOS-drive-stripped shape that Fsfirst/Fsnext
// already use for TNFS directory paths (path_forwardslash, see
// gemdrive_backend_fsfirst()), e.g. N:\CONFIG\SIDETNFS.PRG -> /CONFIG/SIDETNFS.PRG.
//
// Fase 7D-debug: out_raw/out_internal (both nullable) let the caller capture
// two intermediate snapshots for diagnosis, with zero change to the
// resulting tnfs_path -- out_raw is the untouched payload string, out_internal
// is the concatenated-but-not-yet-slash-normalized string right before the
// final back_2_forwardslash()/remove_dup_slashes() pass.
static void __not_in_flash_func(get_tnfs_relative_pathname)(char *tnfs_path, char *out_raw, char *out_internal)
{
    char path_filename[MAX_FOLDER_LENGTH] = {0};
    char tmp_path[MAX_FOLDER_LENGTH] = {0};

    COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, path_filename, MAX_FOLDER_LENGTH);
    if (out_raw)
    {
        snprintf(out_raw, MAX_FOLDER_LENGTH, "%s", path_filename);
    }
    DPRINTF("dpath_string: %s\n", dpath_string);
    DPRINTF("path_filename: %s\n", path_filename);
    if (path_filename[1] == ':')
    {
        snprintf(path_filename, MAX_FOLDER_LENGTH, "%s", path_filename + 2);
        tmp_path[0] = '\0';
    }
    else if (path_filename[0] == '\\')
    {
        tmp_path[0] = '\0';
    }
    else
    {
        if (dpath_string[1] == ':')
        {
            snprintf(tmp_path, MAX_FOLDER_LENGTH, "%s", dpath_string + 2);
        }
        else
        {
            snprintf(tmp_path, MAX_FOLDER_LENGTH, "%s", dpath_string);
        }
        // Fase 7E: this is the one new piece of information not already
        // covered by FOPEN_RAW_PATH/FOPEN_TNFS_PATH (see the FOPEN_CALL
        // case body) -- which current-directory value a relative Fopen
        // path was actually resolved against.
        sidetnfs_diag_log(SIDETNFS_DIAG_PATH_RESOLVE_CWD, 0, dpath_string, NULL, NULL, 0, 0, 0, 0);
    }
    snprintf(tnfs_path, MAX_FOLDER_LENGTH, "%s/%s", tmp_path, path_filename);
    if (out_internal)
    {
        snprintf(out_internal, MAX_FOLDER_LENGTH, "%s", tnfs_path);
    }
    back_2_forwardslash(tnfs_path);
    remove_dup_slashes(tnfs_path);
    DPRINTF("tnfs_path: %s\n", tnfs_path);
}

// Fase 7J-correctie2: compute the parent of an already-canonical (forward-
// slash, get_tnfs_relative_pathname()'d) absolute TNFS path -- "/HALLO" ->
// "/", "/A/B" -> "/A", "/A/B/C" -> "/A/B". Used only to update dpath_string
// (the TNFS CWD) after a successful RMDIR of the directory that was the
// current directory itself -- path is expected to already be the exact
// dpath_string value being replaced, so no further normalization beyond a
// defensive trailing-slash strip is needed.
static void __not_in_flash_func(tnfs_ddelete_parent_path)(const char *path, char *out, size_t out_size)
{
    char tmp[MAX_FOLDER_LENGTH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/')
    {
        tmp[len - 1] = '\0'; // defensive: strip a trailing slash, if any
    }
    char *last_slash = strrchr(tmp, '/');
    if (last_slash == NULL || last_slash == tmp)
    {
        // Either no slash at all (shouldn't happen for a canonical absolute
        // path) or the only slash is the leading one ("/HALLO") -- either
        // way, the parent is root.
        snprintf(out, out_size, "/");
    }
    else
    {
        *last_slash = '\0';
        snprintf(out, out_size, "%s", tmp);
    }
}

// Thin naming wrapper around get_local_full_pathname(). Kept local to this file
// because the wrapped function reads its input implicitly from the file-scope
// globals payloadPtr/dpath_string/hd_folder, not from a parameter.
static void __not_in_flash_func(scfs_get_local_full_pathname)(char *tmp_filepath)
{
    get_local_full_pathname(tmp_filepath);
}

// Declared in scfs.h; implemented here because the GEMDOS_* constants below
// are defined later in this file's own header (gemdrvemul.h), not in scfs.h.
// Mapping matches the values currently used by GEMDRVEMUL_DDELETE_CALL only:
// other handlers (Fdelete, Frename, Fclose) map some FRESULT values
// differently and are intentionally not covered here yet.
int16_t __not_in_flash_func(scfs_fresult_to_gemdos_error)(FRESULT fr)
{
    switch (fr)
    {
    case FR_OK:
        return GEMDOS_EOK;
    case FR_DENIED:
        return GEMDOS_EACCDN;
    case FR_NO_PATH:
        return GEMDOS_EPTHNF;
    default:
        return GEMDOS_EINTRN;
    }
}

// Delete all the file descriptors in the list using delete all files by fdesc
static void __not_in_flash_func(delete_all_files)(FileDescriptors **head)
{
    FileDescriptors *current = *head;
    while (current != NULL)
    {
        FileDescriptors *next = current->next;
        delete_file_by_fdesc(head, current->fd);
        current = next;
    }
}

// Close all the open files, if any. Fase 9E: was unconditionally
// f_close(&current->fobject) for every entry -- harmless only because this
// was previously called solely at the very first PING this Pico boot
// (hd_folder_ready == false), when fdescriptors is always still empty
// under the TNFS backend (every entry added there is
// GEMDRIVE_FILE_BACKEND_TNFS -- see gemdrive_backend_fclose(), whose
// fobject is never populated by a real f_open()). Now also called from
// sidetnfs_probe_reinit_active_server()'s Atari-reset boundary, where TNFS
// files legitimately can be open, so each entry is closed via the correct
// backend -- same distinction gemdrive_backend_fclose() already makes.
static void __not_in_flash_func(close_all_files)(FileDescriptors **head)
{
    FileDescriptors *current = *head;
    while (current != NULL)
    {
        FileDescriptors *next = current->next;
#if SIDETNFS_USE_TNFS_LISTING
        if (current->backend == GEMDRIVE_FILE_BACKEND_TNFS)
        {
            sidetnfs_tnfs_file_close(current->fd, current->tnfs_handle);
            DPRINTF("TNFS file %s closed\n", current->fpath);
        }
#if SIDETNFS_CONFIG_DRIVE_ONLY
        else if (current->backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH)
        {
            // Fase 10B: never opened via f_open() -- current->fobject is
            // never initialized for this backend, so f_close() on it below
            // would be undefined behavior. Nothing to release.
            DPRINTF("Config-flash file %s closed (no-op)\n", current->fpath);
        }
#endif
        else
#endif
        {
            FRESULT fr = f_close(&current->fobject);
            if (fr != FR_OK)
            {
                DPRINTF("ERROR: Could not close file (%d)\r\n", fr);
            }
            else
            {
                DPRINTF("File %s closed successfully\n", current->fpath);
            }
        }
        current = next;
    }
}

// Cound the number of file descriptors in the list
static int __not_in_flash_func(count_fdesc)(FileDescriptors *head)
{
    int count = 0;
    FileDescriptors *current = head;
    while (current != NULL)
    {
        count++;
        current = current->next;
    }
    return count;
}

static void print_variables(uint32_t memory_shared_address)
{
    // DPRINTF("Printing shared variables\n");
    // DPRINTF("GEMDRVEMUL_RANDOM_TOKEN(0x0): %x\n", SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RANDOM_TOKEN))));
    // DPRINTF("GEMDRVEMUL_RANDOM_TOKEN_SEED(0x4): %x\n", SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RANDOM_TOKEN_SEED))));
    DPRINTF("GEMDRVEMUL_TIMEOUT_SEC(0x%04x): %x\n", GEMDRVEMUL_TIMEOUT_SEC, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_TIMEOUT_SEC))));
    DPRINTF("GEMDRVEMUL_PING_STATUS(0x%04x): %x\n", GEMDRVEMUL_PING_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_PING_STATUS))));
    DPRINTF("GEMDRVEMUL_RTC_STATUS(0x%04x): %x\n", GEMDRVEMUL_RTC_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RTC_STATUS))));
    DPRINTF("GEMDRVEMUL_NETWORK_STATUS(0x%04x): %x\n", GEMDRVEMUL_NETWORK_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_NETWORK_STATUS))));
    DPRINTF("GEMDRVEMUL_RTC_ENABLED(0x%04x): %x\n", GEMDRVEMUL_RTC_ENABLED, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RTC_ENABLED))));
    DPRINTF("GEMDRVEMUL_REENTRY_TRAP(0x%04x): %x\n", GEMDRVEMUL_REENTRY_TRAP, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_REENTRY_TRAP))));
    // For strings, swapping is not applicable, so print as is.
    DPRINTF("GEMDRVEMUL_DEFAULT_PATH(0x%04x): %s\n", GEMDRVEMUL_DEFAULT_PATH, (char *)(memory_shared_address + GEMDRVEMUL_DEFAULT_PATH));
    DPRINTF("GEMDRVEMUL_DTA_F_FOUND(0x%04x): %x\n", GEMDRVEMUL_DTA_F_FOUND, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND))));
    DPRINTF("GEMDRVEMUL_DTA_TRANSFER(0x%04x): %x\n", GEMDRVEMUL_DTA_TRANSFER, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DTA_TRANSFER))));
    DPRINTF("GEMDRVEMUL_DTA_EXIST(0x%04x): %x\n", GEMDRVEMUL_DTA_EXIST, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DTA_EXIST))));
    DPRINTF("GEMDRVEMUL_DTA_RELEASE(0x%04x): %x\n", GEMDRVEMUL_DTA_RELEASE, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DTA_RELEASE))));
    DPRINTF("GEMDRVEMUL_SET_DPATH_STATUS(0x%04x): %x\n", GEMDRVEMUL_SET_DPATH_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_SET_DPATH_STATUS))));
    DPRINTF("GEMDRVEMUL_FOPEN_HANDLE(0x%04x): %x\n", GEMDRVEMUL_FOPEN_HANDLE, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FOPEN_HANDLE))));
    DPRINTF("GEMDRVEMUL_READ_BYTES(0x%04x): %x\n", GEMDRVEMUL_READ_BYTES, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_READ_BYTES))));
    DPRINTF("GEMDRVEMUL_READ_BUFF(0x%04x): %x\n", GEMDRVEMUL_READ_BUFF, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_READ_BUFF))));
    DPRINTF("GEMDRVEMUL_WRITE_BYTES(0x%04x): %x\n", GEMDRVEMUL_WRITE_BYTES, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_WRITE_BYTES))));
    DPRINTF("GEMDRVEMUL_WRITE_CHK(0x%04x): %x\n", GEMDRVEMUL_WRITE_CHK, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_WRITE_CHK))));
    DPRINTF("GEMDRVEMUL_WRITE_CONFIRM_STATUS(0x%04x): %x\n", GEMDRVEMUL_WRITE_CONFIRM_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_WRITE_CONFIRM_STATUS))));
    DPRINTF("GEMDRVEMUL_FCLOSE_STATUS(0x%04x): %x\n", GEMDRVEMUL_FCLOSE_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS))));
    DPRINTF("GEMDRVEMUL_DCREATE_STATUS(0x%04x): %x\n", GEMDRVEMUL_DCREATE_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DCREATE_STATUS))));
    DPRINTF("GEMDRVEMUL_DDELETE_STATUS(0x%04x): %x\n", GEMDRVEMUL_DDELETE_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DDELETE_STATUS))));
    DPRINTF("GEMDRVEMUL_FCREATE_HANDLE(0x%04x): %x\n", GEMDRVEMUL_FCREATE_HANDLE, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FCREATE_HANDLE))));
    DPRINTF("GEMDRVEMUL_FDELETE_STATUS(0x%04x): %x\n", GEMDRVEMUL_FDELETE_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FDELETE_STATUS))));
    DPRINTF("GEMDRVEMUL_FSEEK_STATUS(0x%04x): %x\n", GEMDRVEMUL_FSEEK_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FSEEK_STATUS))));
    DPRINTF("GEMDRVEMUL_FATTRIB_STATUS(0x%04x): %x\n", GEMDRVEMUL_FATTRIB_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FATTRIB_STATUS))));
    DPRINTF("GEMDRVEMUL_FRENAME_STATUS(0x%04x): %x\n", GEMDRVEMUL_FRENAME_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS))));
    DPRINTF("GEMDRVEMUL_FDATETIME_DATE(0x%04x): %x\n", GEMDRVEMUL_FDATETIME_DATE, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FDATETIME_DATE))));
    DPRINTF("GEMDRVEMUL_FDATETIME_TIME(0x%04x): %x\n", GEMDRVEMUL_FDATETIME_TIME, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FDATETIME_TIME))));
    DPRINTF("GEMDRVEMUL_FDATETIME_STATUS(0x%04x): %x\n", GEMDRVEMUL_FDATETIME_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FDATETIME_STATUS))));
    DPRINTF("GEMDRVEMUL_DFREE_STATUS(0x%04x): %x\n", GEMDRVEMUL_DFREE_STATUS, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DFREE_STATUS))));
    DPRINTF("GEMDRVEMUL_DFREE_STRUCT(0x%04x): %x\n", GEMDRVEMUL_DFREE_STRUCT, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DFREE_STRUCT))));
    DPRINTF("GEMDRVEMUL_PEXEC_MODE(0x%04x): %x\n", GEMDRVEMUL_PEXEC_MODE, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_PEXEC_MODE))));
    DPRINTF("GEMDRVEMUL_PEXEC_STACK_ADDR(0x%04x): %x\n", GEMDRVEMUL_PEXEC_STACK_ADDR, SWAP_LONGWORD(*((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_PEXEC_STACK_ADDR))));
    // For strings, swapping is not applicable, so print as is.
    DPRINTF("GEMDRVEMUL_PEXEC_FNAME(0x%04x): %s\n", GEMDRVEMUL_PEXEC_FNAME, (char *)(memory_shared_address + GEMDRVEMUL_PEXEC_FNAME));
    DPRINTF("GEMDRVEMUL_PEXEC_CMDLINE(0x%04x): %s\n", GEMDRVEMUL_PEXEC_CMDLINE, (char *)(memory_shared_address + GEMDRVEMUL_PEXEC_CMDLINE));
    DPRINTF("GEMDRVEMUL_PEXEC_ENVSTR(0x%04x): %s\n", GEMDRVEMUL_PEXEC_ENVSTR, (char *)(memory_shared_address + GEMDRVEMUL_PEXEC_ENVSTR));
}

static void print_payload(uint8_t *payloadShowBytesPtr)
{
    // Display the first 256 bytes of the payload in hexadecimal showing 16 bytes per line and the ASCII representation
    for (int i = 0; i < 256 - 10; i += 8)
    {
        uint64_t data = *((uint64_t *)(payloadShowBytesPtr + i));
        DPRINTF("%04x - %02x %02x %02x %02x %02x %02x %02x %02x | %c %c %c %c %c %c %c %c\n",
                i,
                data & 0xFF,
                (data >> 8) & 0xFF,
                (data >> 16) & 0xFF,
                (data >> 24) & 0xFF,
                (data >> 32) & 0xFF,
                (data >> 40) & 0xFF,
                (data >> 48) & 0xFF,
                (data >> 56) & 0xFF,
                (data & 0xFF >= 32 && data & 0xFF <= 126) ? data & 0xFF : '.',
                ((data >> 8) & 0xFF >= 32 && (data >> 8) & 0xFF <= 126) ? (data >> 8) & 0xFF : '.',
                ((data >> 16) & 0xFF >= 32 && (data >> 16) & 0xFF <= 126) ? (data >> 16) & 0xFF : '.',
                ((data >> 24) & 0xFF >= 32 && (data >> 24) & 0xFF <= 126) ? (data >> 24) & 0xFF : '.',
                ((data >> 32) & 0xFF >= 32 && (data >> 32) & 0xFF <= 126) ? (data >> 32) & 0xFF : '.',
                ((data >> 40) & 0xFF >= 32 && (data >> 40) & 0xFF <= 126) ? (data >> 40) & 0xFF : '.',
                ((data >> 48) & 0xFF >= 32 && (data >> 48) & 0xFF <= 126) ? (data >> 48) & 0xFF : '.',
                ((data >> 56) & 0xFF >= 32 && (data >> 56) & 0xFF <= 126) ? (data >> 56) & 0xFF : '.');
    }
}

static void init_variables(uint32_t memory_shared_address)
{
    DPRINTF("Initializing shared variables\n");
    for (uint32_t i = 0; i < 4096; i++)
    {
        *((volatile uint32_t *)(memory_shared_address + (i * 4))) = 0;
    }
}

static void set_shared_var(uint32_t p_shared_variable_index, uint32_t p_shared_variable_value, uint32_t memory_shared_address)
{
    DPRINTF("Setting shared variable %d to %x\n", p_shared_variable_index, p_shared_variable_value);
    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_SHARED_VARIABLES + (p_shared_variable_index * 4) + 2)) = p_shared_variable_value & 0xFFFF;
    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_SHARED_VARIABLES + (p_shared_variable_index * 4))) = p_shared_variable_value >> 16;
}

static void get_shared_var(uint32_t p_shared_variable_index, uint32_t *p_shared_variable_value, uint32_t memory_shared_address)
{
    *p_shared_variable_value = *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_SHARED_VARIABLES + (p_shared_variable_index * 4))) << 16;
    *p_shared_variable_value |= *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_SHARED_VARIABLES + (p_shared_variable_index * 4) + 2));
    DPRINTF("Getting shared variable %d with value %x\n", p_shared_variable_index, *p_shared_variable_value);
}

inline const char *__not_in_flash_func(get_command_name)(unsigned int value)
{
    for (int i = 0; i < numCommands; i++)
    {
        if (commandStr[i].value == value)
        {
            return commandStr[i].name;
        }
    }
    return "COMMAND NOT DEFINED";
}

static inline void __not_in_flash_func(handle_protocol_command)(const TransmissionProtocol *protocol)
{
    if (active_command_id == 0xFFFF)
    {
        payloadPtr = (uint16_t *)protocol->payload + 2;
        DPRINTF("Command %s(%i) received: %d\n", get_command_name(protocol->command_id), protocol->command_id, protocol->payload_size);
        generate_random_token_seed(protocol);
        active_command_id = protocol->command_id;
    }
}

// Interrupt handler callback for DMA completion
void __not_in_flash_func(gemdrvemul_dma_irq_handler_lookup_callback)(void)
{
    // Read the address to process
    uint32_t addr = (uint32_t)dma_hw->ch[lookup_data_rom_dma_channel].al3_read_addr_trig;

    // Avoid priting anything inside an IRQ handled function
    // DPRINTF("DMA LOOKUP: $%x\n", addr);
    if (addr >= ROM3_START_ADDRESS)
    {
        parse_protocol((uint16_t)(addr & 0xFFFF), handle_protocol_command);
    }

    // Clear the interrupt request for the channel
    dma_hw->ints1 = 1u << lookup_data_rom_dma_channel;
}

// Fase 6G: backend-routing helpers for GEMDRIVE directory listing --
// GEMDRVEMUL_FSFIRST_CALL/FSNEXT_CALL/DTA_EXIST_CALL/DTA_RELEASE_CALL each
// call exactly one of these, which internally dispatch on
// SIDETNFS_BACKEND_TYPE (SIDETNFS_USE_TNFS_LISTING/SIDETNFS_USE_SD_LISTING).
// This is a pure "extract function" refactor -- every line of backend
// logic below is unchanged from its previous inline location in the
// switch-case; only the generic parts (payload parsing, DTA/token/return
// writes, active_command_id reset) stay in the case itself. See report/
// SIDETNFS_PHASE5_DIRECTORY_LISTING.md for why the TNFS-backend body looks
// the way it does; BACKEND_FLASH is not implemented (see
// SIDETNFS_BACKEND_FLASH #error in sidetnfs_probe.h).

// Backend for GEMDRVEMUL_DTA_EXIST_CALL. Returns whether ndta has an active
// search under the current backend (TNFS DTA-registry + fake no-network
// listing, or the FatFS DTA table) -- does not itself write to shared
// memory; the caller does that (generic).
static bool gemdrive_backend_dta_exists(uint32_t ndta)
{
    sidetnfs_diag_log(SIDETNFS_DIAG_DTA_EXIST_ENTER, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    bool ndta_exists;
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
    // Fase 10B2: the config drive has its own, separate search registry
    // (sidetnfs_config_drive_backend.c) -- never falls through to the TNFS
    // DTA registry, the fake no-network listing, or the FatFS DTA table,
    // none of which Fsfirst ever populates in this build.
    ndta_exists = sidetnfs_config_drive_search_is_active(ndta);
#else
    // Fase 5Z: DTA_EXIST must recognize TNFS (and fake no-network)
    // search state too -- the 68k side queries this between
    // Fsfirst and Fsnext to decide whether to issue a real Fsnext
    // at all (see report: the SD-baseline log showed COMMAND_ENTER
    // DTA_EXIST immediately before FSNEXT_ENTER). Never touches the
    // FatFS DTA table while this switch is on -- Fsfirst never
    // populates it in this mode.
    bool fake_active = sidetnfs_fake_search_is_active(ndta);
    bool tnfs_active = sidetnfs_tnfs_dta_is_active(ndta);
    ndta_exists = tnfs_active || fake_active;
    if (tnfs_active)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DTA_EXIST_TNFS_OK, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
    else if (fake_active)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DTA_EXIST_FAKE_OK, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
    else
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DTA_EXIST_FAIL, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
    ndta_exists = lookupDTA(ndta) ? true : false;
    sidetnfs_diag_log(ndta_exists ? SIDETNFS_DIAG_DTA_EXIST_FATFS_OK : SIDETNFS_DIAG_DTA_EXIST_FAIL, ndta,
                       NULL, NULL, NULL, 0, 0, 0, 0);
#endif
    DPRINTF("DTA %x exists: %s\n", ndta, (ndta_exists) ? "TRUE" : "FALSE");
    return ndta_exists;
}

// Backend for GEMDRVEMUL_DTA_RELEASE_CALL. Releases whichever backend
// actually holds ndta's search state and returns the active-search count
// (used for the GEMDRVEMUL_DTA_RELEASE return value, same as countDTA() did
// for the FatFS-only path).
static uint32_t gemdrive_backend_dta_release(uint32_t ndta, uint32_t memory_shared_address)
{
    uint32_t release_count;
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
    // Fase 10B2: release only the config drive's own search registry --
    // never the TNFS DTA registry, the fake no-network listing, or the
    // FatFS DTA table, none of which Fsfirst ever populates in this build.
    sidetnfs_config_drive_search_close(ndta);
    release_count = sidetnfs_config_drive_search_count_active();
    nullify_dta(memory_shared_address);
#else
    // Fase 5Z: release whichever backend actually holds ndta's
    // search state -- TNFS DTA registry and/or the fake no-network
    // search table (never the FatFS DTA table, which Fsfirst never
    // populated in this mode). Each release is a no-op if ndta
    // isn't active in that particular table.
    bool was_tnfs_active = sidetnfs_tnfs_dta_is_active(ndta);
    sidetnfs_tnfs_dta_release(ndta);
    bool was_fake_active = sidetnfs_fake_search_is_active(ndta);
    sidetnfs_fake_search_close(ndta);
    if (was_tnfs_active)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DTA_RELEASE_TNFS, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
    if (was_fake_active)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_DTA_RELEASE_FAKE, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
    release_count = sidetnfs_tnfs_dta_count_active() + sidetnfs_fake_search_count_active();
    nullify_dta(memory_shared_address);
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
    DTANode *dtaNode = lookupDTA(ndta);
    if (dtaNode != NULL)
    {
        releaseDTA(ndta);
        DPRINTF("Existing DTA at %x released. DTA table elements: %d\n", ndta, countDTA());
        sidetnfs_diag_log(SIDETNFS_DIAG_DTA_RELEASE_FATFS, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
    nullify_dta(memory_shared_address);
    release_count = countDTA();
#endif
    return release_count;
}

// Backend for GEMDRVEMUL_FSFIRST_CALL. path_forwardslash/internal_path/
// pattern/attribs were already computed by the caller (seach_path_2_st()
// et al. -- generic, backend-independent path/pattern parsing). Writes the
// DTA transfer area and GEMDRVEMUL_DTA_F_FOUND directly (same as before
// extraction); the caller only does the generic write_random_token/
// active_command_id reset afterwards.
static void gemdrive_backend_fsfirst(uint32_t ndta, const char *path_forwardslash, const char *internal_path,
                                       char *pattern, uint32_t attribs, uint32_t memory_shared_address,
                                       bool sidetnfs_network_ok)
{
#if SIDETNFS_USE_TNFS_LISTING
    (void)internal_path; // only used by the SD/FatFS backend below
#if SIDETNFS_CONFIG_DRIVE_ONLY
    (void)sidetnfs_network_ok; // unused this phase -- always RAM-only
    // Fase 10B: root-only, read-only drive. Same trailing-slash
    // normalization as the real-TNFS path below, so "/" and "" both reach
    // sidetnfs_config_drive_search_start() identically; anything else
    // (a subdirectory reference) is a clean GEMDOS "not found", never a
    // fictitious/skipped entry.
    char config_path[MAX_FOLDER_LENGTH];
    snprintf(config_path, sizeof(config_path), "%s", path_forwardslash);
    size_t config_path_len = strlen(config_path);
    if (config_path_len > 1 && config_path[config_path_len - 1] == '/')
    {
        config_path[config_path_len - 1] = '\0';
    }
    SidetnfsDirSearchResult config_result = SIDETNFS_DIR_SEARCH_NOT_FOUND;
    SidetnfsAtariDirEntry config_entry;
    if (config_path[0] == '\0' || strcmp(config_path, "/") == 0)
    {
        config_result = sidetnfs_config_drive_search_start(ndta, pattern, (uint8_t)attribs, &config_entry);
    }
    if (config_result == SIDETNFS_DIR_SEARCH_FOUND)
    {
        populate_dta_from_sidetnfs_entry(memory_shared_address, &config_entry);
    }
    else
    {
        nullify_dta(memory_shared_address);
        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND)) = (uint16_t)GEMDOS_EFILNF;
    }
    return;
#else
    sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_ATTR_PREP, ndta, NULL, NULL, NULL, 0, 0, 0, (uint8_t)attribs);

    // Fase 5Q: directory listing is now UNCONDITIONALLY served from
    // memory (TNFS cache, or the fake no-network listing below) --
    // never from FatFS/SD.
    char tnfs_path[MAX_FOLDER_LENGTH];
    snprintf(tnfs_path, sizeof(tnfs_path), "%s", path_forwardslash);
    size_t tnfs_path_len = strlen(tnfs_path);
    if (tnfs_path_len > 1 && tnfs_path[tnfs_path_len - 1] == '/')
    {
        tnfs_path[tnfs_path_len - 1] = '\0';
    }

    SidetnfsAtariDirEntry sidetnfs_entry;
    SidetnfsDirSearchResult sidetnfs_result;
    if (sidetnfs_network_ok && sidetnfs_tnfs_listing_ready())
    {
        // Fase 5Y/6B: TNFS DTA-registry search (OPENDIRX +
        // one-entry-at-a-time READDIRX, registered under ndta
        // the same way SD's insertDTA()/lookupDTA() works).
        // Never SD.
        sidetnfs_result = sidetnfs_tnfs_dta_start(ndta, tnfs_path, pattern, (uint8_t)attribs, &sidetnfs_entry);
    }
    else
    {
        // Fase 5Q: TNFS never became available this boot (no
        // WiFi, ESC-skip, or MOUNT never succeeded). Serve a
        // fake, memory-only listing instead of any SD fallback
        // -- this NEVER touches cyw43/lwIP (which may already be
        // torn down in this case, see Fase 5H).
        sidetnfs_result = sidetnfs_fake_search_start(ndta, tnfs_path, pattern, (uint8_t)attribs, &sidetnfs_entry);
    }

    if (sidetnfs_result == SIDETNFS_DIR_SEARCH_FOUND)
    {
        populate_dta_from_sidetnfs_entry(memory_shared_address, &sidetnfs_entry);
        sidetnfs_note_tnfs_fs_hit();
        sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_FOUND, ndta, tnfs_path, pattern, sidetnfs_entry.name, 0, 0,
                           0, sidetnfs_entry.attr);
    }
    else // NOT_FOUND (empty/exhausted listing) or ERROR
         // (cache-miss refresh timed out/failed) -- either way, a
         // clean GEMDOS answer, never SD fallback.
    {
        nullify_dta(memory_shared_address);
        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND)) = (uint16_t)GEMDOS_EFILNF;
        if (sidetnfs_result == SIDETNFS_DIR_SEARCH_ERROR)
        {
            sidetnfs_note_tnfs_fs_error();
        }
        else
        {
            sidetnfs_note_tnfs_fs_hit();
        }
        sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_NOT_FOUND, ndta, tnfs_path, pattern, NULL, 0, 0,
                           (uint8_t)sidetnfs_result, 0);
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_RETURN, ndta, NULL, NULL, NULL, 0, 0,
                       (uint8_t)(sidetnfs_result == SIDETNFS_DIR_SEARCH_FOUND ? 0 : GEMDOS_EFILNF), 0);
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
    (void)path_forwardslash;   // only used by the TNFS backend above
    (void)sidetnfs_network_ok; // only used by the TNFS backend above
    bool ndta_exists = lookupDTA(ndta) ? true : false;

    // Fase 5X: SD-baseline measurement -- logging only, no behavior
    // change (see report).
    sidetnfs_diag_log(ndta_exists ? SIDETNFS_DIAG_DTA_LOOKUP_OK : SIDETNFS_DIAG_DTA_LOOKUP_FAIL, ndta, NULL,
                       NULL, NULL, 0, 0, 0, 0);
    sidetnfs_diag_log(ndta_exists ? SIDETNFS_DIAG_SD_DTA_LOOKUP_OK : SIDETNFS_DIAG_SD_DTA_LOOKUP_FAIL, ndta,
                       NULL, NULL, NULL, 0, 0, 0, 0);
    sidetnfs_diag_log(SIDETNFS_DIAG_SD_FIND_FIRST, ndta, internal_path, pattern, NULL, 0, 0, 0,
                       (uint8_t)attribs);

    FRESULT fr;   /* Return value */
    DIR *dj;      /* Directory object */
    FILINFO *fno; /* File information */
    dj = (DIR *)malloc(sizeof(DIR));
    fno = (FILINFO *)malloc(sizeof(FILINFO));

    char raw_filename[2] = "._";
    fr = FR_OK;
    bool first_time = true;
    while (fr == FR_OK && ((raw_filename[0] == '.') || (raw_filename[0] == '.' && raw_filename[1] == '_')))
    {
        if (first_time)
        {
            first_time = false;
            fr = f_findfirst(dj, fno, internal_path, pattern);
        }
        else
        {
            fr = f_findnext(dj, fno);
        }
        if (fno->fname[0])
        {
            if (attribs & attribs_fat2st(fno->fattrib))
            {
                if (fr == FR_OK)
                {
                    raw_filename[0] = fno->fname[0];
                    raw_filename[1] = fno->fname[1];
                }
            }
        }
        else
        {
            raw_filename[0] = 'x'; // Force exit, no more elements
            raw_filename[1] = 'x'; // Force exit, no more elements
        }
    }

    if (fr == FR_OK && fno->fname[0])
    {
        uint8_t attribs_conv_st = attribs_fat2st(fno->fattrib);
        char attribs_str[7] = "";
        get_attribs_st_str(attribs_str, attribs_conv_st);
        char shorten_filename[14];
        char upper_filename[14];
        char filtered_filename[14];
        filter_fname(fno->fname, filtered_filename);
        upper_fname(filtered_filename, upper_filename);
        shorten_fname(upper_filename, shorten_filename);

        strcpy(fno->fname, shorten_filename);

        // Filter out elements that do not match the attributes
        if (attribs_conv_st & attribs)
        {
            DPRINTF("Found: %s, attr: %s\n", fno->fname, attribs_str);
            if (ndta_exists)
            {
                releaseDTA(ndta);
                DPRINTF("Existing DTA at %x released. DTA table elements: %d\n", ndta, countDTA());
                nullify_dta(memory_shared_address);
            }
            DTA data = {"filename.typ", 0, 0, 0, 0, 0, 0, 0, 0, "filename.typ"};
            insertDTA(ndta, data, dj, fno, attribs);
            sidetnfs_diag_log(SIDETNFS_DIAG_DTA_INSERT, ndta, NULL, NULL, fno->fname, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_SD_DTA_INSERT, ndta, NULL, NULL, fno->fname, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_FOUND, ndta, NULL, NULL, fno->fname, 0, 0, 0,
                               attribs_conv_st);
            // Populate the DTA with the first file found
            populate_dta(memory_shared_address, ndta, GEMDOS_EFILNF);
            // Null dj and fno to avoid freeing them
            dj = NULL;
            fno = NULL;
        }
        else
        {
            DPRINTF("Skipped: %s, attr: %s\n", fno->fname, attribs_str);
            sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_NOT_FOUND, ndta, NULL, NULL, fno->fname, 0, 0,
                               (uint8_t)GEMDOS_EFILNF, 0);
            int16_t error_code = GEMDOS_EFILNF;
            DPRINTF("DTA at %x showing error code: %x\n", ndta, error_code);
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND)) = error_code;
            if (ndta_exists)
            {
                releaseDTA(ndta);
                DPRINTF("Existing DTA at %x released. DTA table elements: %d\n", ndta, countDTA());
                nullify_dta(memory_shared_address);
            }
        }
    }
    else
    {
        DPRINTF("Nothing returned from Fsfirst\n");
        int16_t error_code = GEMDOS_EFILNF;
        DPRINTF("DTA at %x showing error code: %x\n", ndta, error_code);
        if (ndta_exists)
        {
            releaseDTA(ndta);
            DPRINTF("Existing DTA at %x released. DTA table elements: %d\n", ndta, countDTA());
        }
        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND)) = error_code;
        nullify_dta(memory_shared_address);
        sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_NOT_FOUND, ndta, NULL, NULL, NULL, 0, 0, (uint8_t)error_code,
                           0);
    }
    // Guarantee that the dynamic memory is released
    if (dj != NULL)
    {
        free(dj);
    }
    if (fno != NULL)
    {
        free(fno);
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_RETURN, ndta, NULL, NULL, NULL, 0, 0,
                       (uint8_t)(*((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND))),
                       0);
#endif
}

// Backend for GEMDRVEMUL_FSNEXT_CALL. Writes the DTA transfer area and
// GEMDRVEMUL_DTA_F_FOUND directly (same as before extraction); the caller
// only does the generic write_random_token/active_command_id reset
// afterwards.
static void gemdrive_backend_fsnext(uint32_t ndta, uint32_t memory_shared_address)
{
#if SIDETNFS_USE_SD_LISTING
    // Fase 5X: SD-baseline measurement -- the TNFS build already
    // logs FSNEXT_ENTER/FSNEXT_CASE_REACHED below (inside the
    // SIDETNFS_USE_TNFS_LISTING block); this is the same pair of
    // events for the SD/FatFS baseline build, logged before any
    // other logic so there is no ambiguity about whether
    // GEMDRVEMUL_FSNEXT_CALL was ever dispatched to at all.
    sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_ENTER, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_CASE_REACHED, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
#endif

#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
    // Fase 10B: root-only, read-only drive -- continue the ndta search
    // started by gemdrive_backend_fsfirst()'s own SIDETNFS_CONFIG_DRIVE_ONLY
    // branch. No TNFS DTA registry, no fake no-network listing, no SD.
    {
        SidetnfsAtariDirEntry config_entry;
        SidetnfsDirSearchResult config_result = sidetnfs_config_drive_search_next(ndta, &config_entry);
        if (config_result == SIDETNFS_DIR_SEARCH_FOUND)
        {
            populate_dta_from_sidetnfs_entry(memory_shared_address, &config_entry);
        }
        else
        {
            nullify_dta(memory_shared_address);
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND)) = (uint16_t)GEMDOS_ENMFIL;
        }
    }
    return;
#else
    // Fase 5U: log both events at the absolute top of this case,
    // before any other logic, so there is no ambiguity about
    // whether GEMDRVEMUL_FSNEXT_CALL was ever dispatched to at all
    // (see report -- hardware testing never showed either of
    // these, which is the actual root cause under investigation).
    sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_ENTER, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_CASE_REACHED, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    // Fase 5Q/6B: directory listing is UNCONDITIONALLY served from
    // memory -- a real TNFS DTA-registry search, or a fake
    // no-network search (see sidetnfs_fake_search_start()). Never
    // falls through to the FatFS DTA hash table below --
    // lookupDTA()/f_findnext() are unreachable here while this
    // switch is on, even if no search is (or is no longer) active
    // for this ndta (a clean GEMDOS_ENMFIL, not a crash).
    SidetnfsAtariDirEntry sidetnfs_entry;
    SidetnfsDirSearchResult sidetnfs_result;
    bool sidetnfs_search_was_active;
    // A search active for ndta is either the fake no-network
    // listing or a real TNFS DTA-registry search -- check both.
    if (sidetnfs_fake_search_is_active(ndta))
    {
        sidetnfs_search_was_active = true;
        sidetnfs_result = sidetnfs_fake_search_next(ndta, &sidetnfs_entry);
    }
    else if (sidetnfs_tnfs_dta_is_active(ndta))
    {
        sidetnfs_search_was_active = true;
        sidetnfs_result = sidetnfs_tnfs_dta_next(ndta, &sidetnfs_entry);
    }
    else
    {
        sidetnfs_search_was_active = false;
        sidetnfs_result = SIDETNFS_DIR_SEARCH_ERROR;
    }
    if (sidetnfs_search_was_active)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_SEARCH_ACTIVE, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
    else
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_SEARCH_MISSING, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
    if (sidetnfs_result == SIDETNFS_DIR_SEARCH_FOUND)
    {
        populate_dta_from_sidetnfs_entry(memory_shared_address, &sidetnfs_entry);
        sidetnfs_note_tnfs_fs_hit();
        sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_FOUND, ndta, NULL, NULL, sidetnfs_entry.name, 0, 0, 0,
                           sidetnfs_entry.attr);
    }
    else
    {
        // NOT_FOUND (listing exhausted) or ERROR (no/invalidated
        // search state): end cleanly with GEMDOS_ENMFIL either
        // way -- no SD fallback.
        sidetnfs_fake_search_close(ndta); // no-op if this was a real TNFS search, not the fake listing
        sidetnfs_tnfs_dta_release(ndta);  // no-op if this was the fake listing, not a real TNFS search
        nullify_dta(memory_shared_address);
        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND)) = (uint16_t)GEMDOS_ENMFIL;
        if (sidetnfs_result == SIDETNFS_DIR_SEARCH_ERROR)
        {
            sidetnfs_note_tnfs_fs_error();
        }
        else
        {
            sidetnfs_note_tnfs_fs_hit();
        }
        sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_END, ndta, NULL, NULL, NULL, 0, 0, (uint8_t)sidetnfs_result,
                           0);
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_RETURN, ndta, NULL, NULL, NULL, 0, 0,
                       (uint8_t)(sidetnfs_result == SIDETNFS_DIR_SEARCH_FOUND ? 0 : GEMDOS_ENMFIL), 0);
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
    FRESULT fr; /* Return value */
    DTANode *dtaNode = lookupDTA(ndta);

    bool ndta_exists = dtaNode ? true : false;
    // Fase 5X: SD-baseline measurement -- logging only, no behavior
    // change (see report). This is the lookupDTA() call whose
    // success/failure directly answers "does Fsnext find the DTA
    // Fsfirst inserted?".
    sidetnfs_diag_log(ndta_exists ? SIDETNFS_DIAG_DTA_LOOKUP_OK : SIDETNFS_DIAG_DTA_LOOKUP_FAIL, ndta, NULL,
                       NULL, NULL, 0, 0, 0, 0);
    sidetnfs_diag_log(ndta_exists ? SIDETNFS_DIAG_SD_DTA_LOOKUP_OK : SIDETNFS_DIAG_SD_DTA_LOOKUP_FAIL, ndta,
                       NULL, NULL, NULL, 0, 0, 0, 0);
    if (ndta_exists)
    {
        sidetnfs_diag_log(SIDETNFS_DIAG_SD_FIND_NEXT, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
    }
    if (dtaNode != NULL && dtaNode->dj != NULL && dtaNode->fno != NULL && ndta_exists)
    {
        uint32_t attribs = dtaNode->attribs;
        // We need to filter out the elements that does not make sense in the FsFat environment
        // And in the Atari ST environment
        char raw_filename[2] = "._";
        fr = FR_OK;
        while (fr == FR_OK && ((raw_filename[0] == '.') || (raw_filename[0] == '.' && raw_filename[1] == '_')))
        {
            fr = f_findnext(dtaNode->dj, dtaNode->fno);
            DPRINTF("Fsnext fr: %d and filename: %s\n", fr, dtaNode->fno->fname);
            if (dtaNode->fno->fname[0])
            {
                if (attribs & attribs_fat2st(dtaNode->fno->fattrib))
                {
                    if (fr == FR_OK)
                    {
                        raw_filename[0] = dtaNode->fno->fname[0];
                        raw_filename[1] = dtaNode->fno->fname[1];
                    }
                }
            }
            else
            {
                raw_filename[0] = 'X'; // Force exit, no more elements
                raw_filename[1] = 'X'; // Force exit, no more elements
            }
        }
        if (fr == FR_OK && dtaNode->fno->fname[0])
        {
            char shorten_filename[14];
            char upper_filename[14];
            char filtered_filename[14];
            filter_fname(dtaNode->fno->fname, filtered_filename);
            upper_fname(filtered_filename, upper_filename);
            shorten_fname(upper_filename, shorten_filename);
            strcpy(dtaNode->fno->fname, shorten_filename);

            uint8_t attribs = dtaNode->fno->fattrib;
            uint8_t attribs_conv_st = attribs_fat2st(attribs);
            if (!(attribs & (FS_ST_LABEL)))
            {
                attribs |= FS_ST_ARCH;
            }
            char attribs_str[7] = "";
            get_attribs_st_str(attribs_str, attribs_conv_st);
            DPRINTF("Found: %s, attr: %s\n", dtaNode->fno->fname, attribs_str);
            sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_FOUND, ndta, NULL, NULL, dtaNode->fno->fname, 0, 0, 0,
                               attribs_conv_st);
            // Populate the DTA with the next file found
            populate_dta(memory_shared_address, ndta, GEMDOS_ENMFIL);
        }
        else
        {
            DPRINTF("Nothing found\n");
            int16_t error_code = GEMDOS_ENMFIL;
            DPRINTF("DTA at %x showing error code: %x\n", ndta, error_code);
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND)) = error_code;
            if (ndta_exists)
            {
                releaseDTA(ndta);
                DPRINTF("Existing DTA at %x released. DTA table elements: %d\n", ndta, countDTA());
            }
            nullify_dta(memory_shared_address);
            sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_END, ndta, NULL, NULL, NULL, 0, 0, (uint8_t)error_code, 0);
        }
    }
    else
    {
        DPRINTF("FsFirst not initalized\n");
        int16_t error_code = GEMDOS_EINTRN;
        DPRINTF("DTA at %x showing error code: %x\n", ndta, error_code);
        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND)) = error_code;
        if (ndta_exists)
        {
            releaseDTA(ndta);
            DPRINTF("Existing DTA at %x released. DTA table elements: %d\n", ndta, countDTA());
        }
        nullify_dta(memory_shared_address);
        sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_END, ndta, NULL, NULL, NULL, 0, 0, (uint8_t)error_code, 0);
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FSNEXT_RETURN, ndta, NULL, NULL, NULL, 0, 0,
                       (uint8_t)(*((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DTA_F_FOUND))),
                       0);
#endif
}

#if SIDETNFS_CONFIG_DRIVE_ONLY
// Fase 10B: tmp_filepath at this point is the TNFS-relative, forward-slash,
// root-anchored shape get_tnfs_relative_pathname() always produces (e.g.
// "/SIDETNFS.PRG"). This drive is root-only, so accept exactly one path
// component after the leading slash -- reject an empty name and reject
// any further slash (a subdirectory reference) rather than silently
// truncating it to something that could accidentally match a real file.
static bool config_flash_root_name(const char *tmp_filepath, char *out83, size_t out83_size)
{
    if (tmp_filepath == NULL || tmp_filepath[0] != '/')
    {
        return false;
    }
    const char *name = tmp_filepath + 1;
    if (name[0] == '\0' || strchr(name, '/') != NULL)
    {
        return false;
    }
    strncpy(out83, name, out83_size - 1);
    out83[out83_size - 1] = '\0';
    return true;
}
#endif // SIDETNFS_CONFIG_DRIVE_ONLY

// Fase 7D: GEMDRVEMUL_FOPEN_CALL backend routing. tmp_filepath is already
// the correct shape for the active backend (TNFS-relative forward-slash
// path, or SD-local hd_folder-prefixed path -- built by the caller before
// this is called, see get_tnfs_relative_pathname()/get_local_full_pathname()
// at the GEMDRVEMUL_FOPEN_CALL call site).
static void gemdrive_backend_fopen(uint16_t fopen_mode, const char *tmp_filepath, uint32_t memory_shared_address)
{
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
    // Fase 10B: read-only drive -- only mode 0 (read-only) is ever valid.
    if (fopen_mode != 0)
    {
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, GEMDOS_EACCDN);
        return;
    }
    char name83[32];
    const uint8_t *data;
    uint32_t size;
    if (!config_flash_root_name(tmp_filepath, name83, sizeof(name83)) ||
        !sidetnfs_config_drive_lookup(name83, &data, &size))
    {
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, GEMDOS_EFILNF);
        return;
    }
    uint16_t fd_counter = get_first_available_fd(fdescriptors);
    FileDescriptors *newFDescriptor = malloc(sizeof(FileDescriptors));
    if (newFDescriptor == NULL)
    {
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, GEMDOS_EINTRN);
        return;
    }
    add_config_flash_file(&fdescriptors, newFDescriptor, tmp_filepath, data, size, fd_counter);
    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, fd_counter);
    return;
#else
    sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_ENTER, 0, tmp_filepath, NULL, NULL, fopen_mode, 0, 0, 0);
    if (fopen_mode > 2)
    {
        // Fase 7K: only GEMDOS modes 0 (read-only), 1 (write-only) and 2
        // (read/write) are defined by Fopen -- anything else is denied
        // locally, same as before. Modes 1/2 themselves are no longer
        // denied here (Fase 7C/7D's blanket "write not implemented" denial
        // is superseded now that TNFS OPEN is sent with write-capable
        // flags below -- see sidetnfs_tnfs_file_open()).
        DPRINTF("ERROR: TNFS backend denies fopen mode: %x\n", fopen_mode);
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_DENY_MODE, 0, tmp_filepath, NULL, NULL, fopen_mode, 0, 0, 0);
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_RETURN, 0, tmp_filepath, NULL, NULL, 0, 0, (uint8_t)GEMDOS_EACCDN, 0);
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, GEMDOS_EACCDN);
        return;
    }
    uint8_t tnfs_handle = 0;
    SidetnfsFileOpenResult result = sidetnfs_tnfs_file_open(tmp_filepath, fopen_mode, &tnfs_handle);
    if (result != SIDETNFS_FILE_OPEN_OK)
    {
        DPRINTF("ERROR: Could not open TNFS file %s (result %d)\n", tmp_filepath, (int)result);
        int32_t gemdos_rc = result == SIDETNFS_FILE_OPEN_NOT_FOUND ? GEMDOS_EFILNF : GEMDOS_EINTRN;
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_RETURN, 0, tmp_filepath, NULL, NULL, 0, 0, (uint8_t)gemdos_rc, 0);
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, gemdos_rc);
        return;
    }
    uint16_t fd_counter = get_first_available_fd(fdescriptors);
    FileDescriptors *newFDescriptor = malloc(sizeof(FileDescriptors));
    if (newFDescriptor == NULL)
    {
        DPRINTF("Memory allocation failed for new FileDescriptors\n");
        // Best-effort -- don't leak the TNFS-side handle just because the
        // local tracking entry couldn't be allocated.
        sidetnfs_tnfs_file_close(fd_counter, tnfs_handle);
        sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_RETURN, 0, tmp_filepath, NULL, NULL, 0, 0, (uint8_t)GEMDOS_EINTRN, 0);
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, GEMDOS_EINTRN);
        return;
    }
    add_tnfs_file(&fdescriptors, newFDescriptor, tmp_filepath, tnfs_handle, fd_counter, fopen_mode != 0);
    DPRINTF("TNFS file opened with file descriptor: %d\n", fd_counter);
    sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_RETURN, fd_counter, tmp_filepath, NULL, NULL, 0, 0, 0, 0);
    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, fd_counter);
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
    DPRINTF("Fopen mode: %x\n", fopen_mode);
    BYTE fatfs_open_mode = 0;
    switch (fopen_mode)
    {
    case 0: // Read only
        fatfs_open_mode = FA_READ;
        break;
    case 1: // Write only
        fatfs_open_mode = FA_WRITE;
        break;
    case 2: // Read/Write
        fatfs_open_mode = FA_READ | FA_WRITE;
        break;
    default:
        DPRINTF("ERROR: Invalid mode: %x\n", fopen_mode);
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, GEMDOS_EACCDN);
        break;
    }
    DPRINTF("FatFs open mode: %x\n", fatfs_open_mode);
    if (fopen_mode <= 2)
    {
        // Open the file with FatFs
        FRESULT fr;
        FIL file_object;
        fr = f_open(&file_object, tmp_filepath, fatfs_open_mode);
        if (fr != FR_OK)
        {
            DPRINTF("ERROR: Could not open file (%d)\r\n", fr);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, GEMDOS_EFILNF);
        }
        else
        {
            // Add the file to the list of open files
            int fd_counter = get_first_available_fd(fdescriptors);
            DPRINTF("Opening file with new file descriptor: %d\n", fd_counter);
            FileDescriptors *newFDescriptor = malloc(sizeof(FileDescriptors));
            if (newFDescriptor == NULL)
            {
                DPRINTF("Memory allocation failed for new FileDescriptors\n");
                DPRINTF("ERROR: Could not add file to the list of open files\n");
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, GEMDOS_EINTRN);
            }
            else
            {
                add_file(&fdescriptors, newFDescriptor, tmp_filepath, file_object, fd_counter);
                DPRINTF("File opened with file descriptor: %d\n", fd_counter);
                // Return the file descriptor
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FOPEN_HANDLE, fd_counter);
            }
        }
    }
#endif
}

// Fase 7D: GEMDRVEMUL_FCLOSE_CALL backend routing.
static void gemdrive_backend_fclose(uint16_t fclose_fd, uint32_t memory_shared_address)
{
#if SIDETNFS_USE_TNFS_LISTING
    sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_ENTER, fclose_fd, NULL, NULL, NULL, 0, 0, 0, 0);
    FileDescriptors *file = get_file_by_fdesc(fdescriptors, fclose_fd);
#if SIDETNFS_CONFIG_DRIVE_ONLY
    // Fase 10B: flash reads need no close call at all -- just release the
    // local tracking entry, same "always release, never leak/hang" intent
    // as the TNFS path below.
    if (file == NULL || file->backend != GEMDRIVE_FILE_BACKEND_CONFIG_FLASH)
    {
        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EIHNDL;
        return;
    }
    delete_file_by_fdesc(&fdescriptors, fclose_fd);
    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EOK;
    return;
#else
    if (file == NULL || file->backend != GEMDRIVE_FILE_BACKEND_TNFS)
    {
        DPRINTF("ERROR: File descriptor not found or not TNFS-backed\n");
        sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_BACKEND, fclose_fd, NULL, NULL, NULL, 0, 0,
                           file ? (uint8_t)file->backend : 0xFFu, 0);
        sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_RETURN, fclose_fd, NULL, NULL, NULL, 0, 0, (uint8_t)GEMDOS_EIHNDL, 0);
        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EIHNDL;
        return;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_HANDLE, fclose_fd, NULL, NULL, NULL, file->tnfs_handle, 0, 0, 0);
    sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_BACKEND, fclose_fd, NULL, NULL, NULL, 0, 0, (uint8_t)file->backend, 0);
    // Fase 7D: always release the local descriptor, even if the TNFS-side
    // CLOSE times out or errors -- same "cleanup must never hang or leak"
    // contract as tnfs_dta_closedir() for directory handles (Fase 5AA).
    // Removing the entry here is also what prevents a double-close: a
    // second FCLOSE with the same fd finds file == NULL above.
    sidetnfs_tnfs_file_close(fclose_fd, file->tnfs_handle);
    delete_file_by_fdesc(&fdescriptors, fclose_fd);
    DPRINTF("TNFS file closed\n");
    sidetnfs_diag_log(SIDETNFS_DIAG_FCLOSE_RETURN, fclose_fd, NULL, NULL, NULL, 0, 0, (uint8_t)GEMDOS_EOK, 0);
    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EOK;
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
    FileDescriptors *file = get_file_by_fdesc(fdescriptors, fclose_fd);
    if (file == NULL)
    {
        DPRINTF("ERROR: File descriptor not found\n");
        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EIHNDL;
    }
    else
    {
        // Close the file with FatFs
        FRESULT fr = f_close(&file->fobject);
        if (fr == FR_INVALID_OBJECT)
        {
            DPRINTF("ERROR: File descriptor is not valid\n");
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EIHNDL;
        }
        else if (fr != FR_OK)
        {
            DPRINTF("ERROR: Could not close file (%d)\r\n", fr);
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EINTRN;
        }
        else
        {
            // Remove the file from the list of open files
            delete_file_by_fdesc(&fdescriptors, fclose_fd);
            DPRINTF("File closed\n");
            // Return the file descriptor
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCLOSE_STATUS)) = GEMDOS_EOK;
        }
    }
#endif
}

// Fase 7D: GEMDRVEMUL_READ_BUFF_CALL backend routing.
static void gemdrive_backend_fread(uint16_t readbuff_fd, uint32_t readbuff_pending_bytes_to_read, uint32_t memory_shared_address)
{
#if SIDETNFS_USE_TNFS_LISTING
    sidetnfs_diag_log(SIDETNFS_DIAG_FREAD_ENTER, readbuff_fd, NULL, NULL, NULL, 0,
                       (uint16_t)(readbuff_pending_bytes_to_read > 0xFFFFu ? 0xFFFFu : readbuff_pending_bytes_to_read), 0, 0);
    sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_ENTER, readbuff_fd, NULL, NULL, NULL, 0, 0, 0, 0);
    FileDescriptors *file = get_file_by_fdesc(fdescriptors, readbuff_fd);
#if SIDETNFS_CONFIG_DRIVE_ONLY
    if (file == NULL || file->backend != GEMDRIVE_FILE_BACKEND_CONFIG_FLASH)
    {
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, GEMDOS_EIHNDL);
        return;
    }
    // Fase 10B: bound every read to the real array length. file->offset can
    // be anywhere in [0, config_flash_size] -- Fseek, not just linear
    // growth (Pexec's own PRG loader may not read strictly sequentially) --
    // so `remaining` is recomputed fresh here every call, never assumed to
    // only shrink monotonically from a prior read. No interrupts disabled
    // anywhere in this path (plain memcpy from an already-mapped XIP
    // array), same as every other backend's Fread.
    uint32_t remaining = (file->offset < file->config_flash_size) ? (file->config_flash_size - file->offset) : 0;
    uint16_t buff_size = (uint16_t)(readbuff_pending_bytes_to_read > DEFAULT_FOPEN_READ_BUFFER_SIZE
                                          ? DEFAULT_FOPEN_READ_BUFFER_SIZE
                                          : readbuff_pending_bytes_to_read);
    if (buff_size > remaining)
    {
        buff_size = (uint16_t)remaining;
    }
    if (buff_size < DEFAULT_FOPEN_READ_BUFFER_SIZE)
    {
        memset((void *)(memory_shared_address + GEMDRVEMUL_READ_BUFF), 0, DEFAULT_FOPEN_READ_BUFFER_SIZE);
    }
    if (buff_size > 0)
    {
        memcpy((void *)(memory_shared_address + GEMDRVEMUL_READ_BUFF), file->config_flash_data + file->offset, buff_size);
    }
    file->offset += buff_size;
    CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_READ_BUFF, buff_size + (buff_size % 2));
    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, (uint32_t)buff_size);
    return;
#else
    if (file == NULL || file->backend != GEMDRIVE_FILE_BACKEND_TNFS)
    {
        DPRINTF("ERROR: File descriptor not found or not TNFS-backed\n");
        sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_BACKEND, readbuff_fd, NULL, NULL, NULL, 0, 0,
                           file ? (uint8_t)file->backend : 0xFFu, 0);
        sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_RETURN, readbuff_fd, NULL, NULL, NULL, 0, 0,
                           (uint8_t)GEMDOS_EIHNDL, 0);
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, GEMDOS_EIHNDL);
        return;
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_HANDLE, readbuff_fd, NULL, NULL, NULL, file->tnfs_handle, 0, 0, 0);
    sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_BACKEND, readbuff_fd, NULL, NULL, NULL, 0, 0, (uint8_t)file->backend, 0);
    uint16_t buff_size = readbuff_pending_bytes_to_read > DEFAULT_FOPEN_READ_BUFFER_SIZE
                              ? DEFAULT_FOPEN_READ_BUFFER_SIZE
                              : (uint16_t)readbuff_pending_bytes_to_read;
    sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_REQUESTED, readbuff_fd, NULL, NULL, NULL, 0, buff_size, 0, 0);
    {
        char offset_str[24];
        snprintf(offset_str, sizeof(offset_str), "off=%lu", (unsigned long)file->offset);
        sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_OFFSET_BEFORE, readbuff_fd, offset_str, NULL, NULL, 0, 0, 0, 0);
    }
    if (buff_size < DEFAULT_FOPEN_READ_BUFFER_SIZE)
    {
        memset((void *)(memory_shared_address + GEMDRVEMUL_READ_BUFF), 0, DEFAULT_FOPEN_READ_BUFFER_SIZE);
    }
    uint16_t actual = 0;
    bool ok = sidetnfs_tnfs_file_read(readbuff_fd, file->tnfs_handle,
                                       (uint8_t *)(memory_shared_address + GEMDRVEMUL_READ_BUFF), buff_size, &actual);
    if (!ok)
    {
        DPRINTF("ERROR: Could not read TNFS file (fd %x)\n", readbuff_fd);
        sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_RETURN, readbuff_fd, NULL, NULL, NULL, 0, 0,
                           (uint8_t)GEMDOS_EINTRN, 0);
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, GEMDOS_EINTRN);
        return;
    }
    // Update the offset of the file -- RAM-only bookkeeping (Fseek isn't
    // implemented for TNFS handles this phase, see GEMDRVEMUL_FSEEK_CALL),
    // kept in sync in case a future phase needs it.
    file->offset += actual;
    sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_ACTUAL, readbuff_fd, NULL, NULL, NULL, 0, actual, 0, 0);
    {
        char offset_str[24];
        snprintf(offset_str, sizeof(offset_str), "off=%lu", (unsigned long)file->offset);
        sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_OFFSET_AFTER, readbuff_fd, offset_str, NULL, NULL, 0, 0, 0, 0);
    }
    sidetnfs_diag_log(SIDETNFS_DIAG_READ_BUFF_RETURN, readbuff_fd, NULL, NULL, NULL, 0, actual, 0, 0);
    CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_READ_BUFF, actual + (actual % 2));
    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, (uint32_t)actual);
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
    // Obtain the file descriptor
    FileDescriptors *file = get_file_by_fdesc(fdescriptors, readbuff_fd);
    if (file == NULL)
    {
        DPRINTF("ERROR: File descriptor not found\n");
        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, GEMDOS_EIHNDL);
    }
    else
    {
        uint32_t readbuff_offset = file->offset;
        UINT bytes_read = 0;
        // Read the file with FatFs
        FRESULT fr = f_lseek(&file->fobject, readbuff_offset);
        if (fr != FR_OK)
        {
            DPRINTF("ERROR: Could not change read offset of the file (%d)\r\n", fr);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, GEMDOS_EINTRN);
        }
        else
        {
            // Only read DEFAULT_FOPEN_READ_BUFFER_SIZE bytes at a time
            uint16_t buff_size = readbuff_pending_bytes_to_read > DEFAULT_FOPEN_READ_BUFFER_SIZE ? DEFAULT_FOPEN_READ_BUFFER_SIZE : readbuff_pending_bytes_to_read;
            DPRINTF("Reading x%x bytes from the file at offset x%x\n", buff_size, readbuff_offset);
            if (buff_size < DEFAULT_FOPEN_READ_BUFFER_SIZE) {
                memset((void *)(memory_shared_address + GEMDRVEMUL_READ_BUFF), 0, DEFAULT_FOPEN_READ_BUFFER_SIZE);
            }
            fr = f_read(&file->fobject, (void *)(memory_shared_address + GEMDRVEMUL_READ_BUFF), buff_size, &bytes_read);
            if (fr != FR_OK)
            {
                DPRINTF("ERROR: Could not read file (%d)\r\n", fr);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, GEMDOS_EINTRN);
            }
            else
            {
                // Update the offset of the file
                file->offset += bytes_read;
                uint32_t current_offset = file->offset;
                DPRINTF("New offset: x%x after reading x%x bytes\n", current_offset, bytes_read);
                // Change the endianness of the bytes read
                CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_READ_BUFF, buff_size + (buff_size % 2));
                // Return the number of bytes read
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_READ_BYTES, (uint32_t)bytes_read);
            }
        }
    }
#endif
}

// Fase 9D/9E: single shared drive-letter selection, used both at Pico cold
// boot (init_gemdrvemul()'s startup, below) and at the Atari-reset
// boundary (GEMDRVEMUL_PING's reinit branch, below) -- deliberately one
// implementation, not two, per the report's requirement. Prefers the
// active TNFS drive's own letter (sidetnfs_probe_has_active_server(),
// reflecting whatever sidetnfs_probe_load_active_server() most recently
// loaded from the persistent config); falls back to the legacy
// PARAM_GEMDRIVE_DRIVE config-store entry (default 'C') when no usable
// TNFS/UDP drive is configured -- missing/invalid config must never leave
// GEMDRIVE without a drive letter to boot with.
static char select_gemdrive_drive_letter(void)
{
#if SIDETNFS_CONFIG_DRIVE_ONLY
    // Fase 10B: the config drive is the sole GEMDRIVE backend this build
    // ever offers -- always the persistent config_drive_letter (default
    // 'S', see SIDETNFS_DEFAULT_CONFIG_DRIVE_LETTER), never the TNFS/SD
    // drive-letter selection below. sidetnfs_config_init() runs once at
    // boot, unconditionally, before command dispatch, so this is always
    // valid without SD/WiFi/network.
    uint8_t config_letter = sidetnfs_config_get_config_drive_letter();
    return config_letter != 0 ? (char)config_letter : 'S';
#else
    if (sidetnfs_probe_has_active_server())
    {
        return sidetnfs_probe_get_active_drive_letter();
    }
    ConfigEntry *drive_letter_conf = find_entry(PARAM_GEMDRIVE_DRIVE);
    return (drive_letter_conf != NULL) ? drive_letter_conf->value[0] : 'C';
#endif
}

void init_gemdrvemul(bool safe_config_reboot)
{
    FRESULT fr; /* FatFs function common result code */
    FATFS fs;
    // Fase 8C investigation: hd_folder_ready's actual current meaning is
    // narrower than its name suggests, and does NOT imply SD, WiFi, or
    // TNFS are available. It only means "the one-time GEMDRVEMUL_PING
    // first-call initialization has run" -- resetting fdescriptors/the DTA
    // table/dpath_string, and (only for the SD backend) a successful SD
    // mount. For the TNFS backend it becomes true on the very first PING
    // regardless of whether SD, WiFi, or TNFS actually ended up available
    // (see GEMDRVEMUL_PING below) -- it exists purely so that one-time
    // setup is never repeated on subsequent PINGs, not as a backend- or
    // hardware-readiness signal. Left named as-is (renaming a
    // widely-referenced variable is a bigger, riskier change than this
    // phase's scope calls for) -- use sd_available / sidetnfs_network_ok /
    // sidetnfs_tnfs_listing_ready() for actual backend availability
    // instead, never this flag.
    bool hd_folder_ready = false;

    char *ntp_server_host = NULL;
    int ntp_server_port = NTP_DEFAULT_PORT;
    u_int16_t network_poll_counter = 0;

    // Local wifi password in the local file
    char *wifi_password_file_content = NULL;

    srand(time(0));
    printf("Initializing GEMDRIVE...\n"); // Print alwayse

    dpath_string[0] = '\\'; // Set the root folder as default
    dpath_string[1] = '\0';

    bool write_config_only_once = true;
    active_command_id = 0xFFFF;

    DPRINTF("Waiting for commands...\n");
    uint32_t memory_shared_address = ROM3_START_ADDRESS; // Start of the shared memory buffer
    uint32_t memory_firmware_code = ROM4_START_ADDRESS;  // Start of the firmware code

    init_variables(memory_shared_address);

    *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RTC_STATUS)) = 0x0;
    *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_NETWORK_STATUS)) = 0x0;
    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0x0;


    ConfigEntry *gemdrive_rtc = find_entry(PARAM_GEMDRIVE_RTC);
    bool gemdrive_rtc_enabled = true;
    if (gemdrive_rtc != NULL)
    {
        gemdrive_rtc_enabled = gemdrive_rtc->value[0] == 't' || gemdrive_rtc->value[0] == 'T';
    }
    // #if defined(_DEBUG) && (_DEBUG != 0)
    //     DPRINTF("RTC DISABLED FOR DEBUGGING\n");
    //     gemdrive_rtc_enabled = false;
    // #endif
    *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RTC_ENABLED)) = gemdrive_rtc_enabled;
    DPRINTF("Network enabled? %s\n", gemdrive_rtc_enabled ? "Yes" : "No");

    ConfigEntry *gemdrive_timeout = find_entry(PARAM_GEMDRIVE_TIMEOUT_SEC);
    uint32_t gemdrive_timeout_sec = 0;
    if (gemdrive_timeout != NULL)
    {
        gemdrive_timeout_sec = atoi(gemdrive_timeout->value);
    }
    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_TIMEOUT_SEC, gemdrive_timeout_sec);
    DPRINTF("Timeout in seconds: %d\n", gemdrive_timeout_sec);

    // Fase 9D/9E: see select_gemdrive_drive_letter() above -- the one
    // shared implementation, also used by GEMDRVEMUL_PING's reinit branch.
    char drive_letter = select_gemdrive_drive_letter();
    uint32_t drive_letter_num = (uint8_t)toupper(drive_letter);
    uint32_t drive_number = drive_letter_num - 65; // Convert the drive letter to a number. Add 1 because 0 is the current drive

    ConfigEntry *buffer_type_conf = find_entry(PARAM_GEMDRIVE_BUFF_TYPE);
    uint16_t buffer_type = 0; // 0: Diskbuffer, 1: Stack
    if (buffer_type_conf != NULL)
    {
        buffer_type = atoi(buffer_type_conf->value);
    }

    ConfigEntry *virtual_fake_floppy_conf = find_entry(PARAM_GEMDRIVE_FAKEFLOPPY);
    uint16_t virtual_fake_floppy = 0; // 0: No, 1: Yes
    if (virtual_fake_floppy_conf != NULL)
    {
        virtual_fake_floppy = virtual_fake_floppy_conf->value[0] == 't' || virtual_fake_floppy_conf->value[0] == 'T';
    }

    ConfigEntry *y2k_patch = find_entry(PARAM_RTC_Y2K_PATCH);
    if (y2k_patch != NULL)
    {
        char *str = y2k_patch->value;
        y2k_patch_enabled = ((y2k_patch!=NULL) && (strlen(y2k_patch->value)) && ((str[0] == 'T') || (str[0] == 't'))) ? true : false;
    }
    else {
        y2k_patch_enabled = true;
        DPRINTF("Y2K patch enabled by default\n");
    }
    DPRINTF("Y2K patch enabled: %s\n", y2k_patch_enabled ? "true" : "false");
    *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RTC_Y2K_PATCH)) = y2k_patch_enabled ? 0xFFFFFFFF : 0;

    set_shared_var(SHARED_VARIABLE_FIRST_FILE_DESCRIPTOR, FIRST_FILE_DESCRIPTOR, memory_shared_address);
    set_shared_var(SHARED_VARIABLE_DRIVE_LETTER, drive_letter_num, memory_shared_address);
    set_shared_var(SHARED_VARIABLE_DRIVE_NUMBER, drive_number, memory_shared_address);
    set_shared_var(SHARED_VARIABLE_BUFFER_TYPE, buffer_type, memory_shared_address);
    set_shared_var(SHARED_VARIABLE_FAKE_FLOPPY, virtual_fake_floppy, memory_shared_address);

    for (int i = 0; i < SHARED_VARIABLES_SIZE; i++)
    {
        uint32_t value = *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_SHARED_VARIABLES + (i * 4)));
        DPRINTF("Shared variable %d: %04x%04x\n", i, value & 0xFFFF, value >> 16);
    }

    // Fase 5H: only true once WiFi is confirmed still up (set right before
    // sidetnfs_send_mount_probe() below). Guards every later call that
    // touches cyw43/lwIP in the main loop, since every other path below
    // this point falls back to cyw43_arch_deinit()/network_terminate() --
    // polling a torn-down network stack there previously caused a hang.
    bool sidetnfs_network_ok = false;

    // Only try to get the datetime from the network if the wifi is configured
#if SIDETNFS_CONFIG_DRIVE_ONLY
    // Fase 10B: the config drive needs no SD, WiFi, or TNFS access at all --
    // skip the whole WiFi-connect/NTP/mount-probe block below entirely
    // (never even calls network_init()) and go straight to the same
    // teardown its own "not configured" paths already use, so
    // sidetnfs_network_ok stays false and the drive is reported ready
    // immediately. This mirrors the "no wifi configured" else-branch
    // exactly (see the bottom of this same if/else below), just skipped
    // unconditionally instead of based on PARAM_WIFI_SSID.
    //
    // Fase 10B-afronding: only deinit if main() actually confirmed
    // cyw43_arch_init() succeeded -- checked here instead of assumed, even
    // though main() currently guarantees it for every path that can reach
    // GEMDRIVE_EMULATOR mode (a failed cyw43_arch_init() there returns -1
    // before any emulator mode runs at all).
    if (sidetnfs_cyw43_arch_is_ready())
    {
        cyw43_arch_deinit();
    }
    DPRINTF("SIDETNFS_CONFIG_DRIVE_ONLY build -- skipping network initialization.\n");
    sidetnfs_mark_network_skipped();
#else
    if (gemdrive_rtc_enabled && strlen(find_entry(PARAM_WIFI_SSID)->value) > 0)
    {
#if SIDETNFS_ENABLE_SD_SUPPORT
        // Initialize SD card
        if (!sd_init_driver())
        {
            DPRINTF("ERROR: Could not initialize SD card\r\n");
        }
        else
        {
            FRESULT err = read_and_trim_file(WIFI_PASS_FILE_NAME, &wifi_password_file_content, MAX_WIFI_PASSWORD_LENGTH);
            if (err == FR_OK)
            {
                DPRINTF("Wifi password file found. Content: %s\n", wifi_password_file_content);
            }
            else
            {
                DPRINTF("Wifi password file not found.\n");
            }
        }
#else
        // Fase 8B: SIDETNFS_ENABLE_SD_SUPPORT==0 -- no SD-based WiFi
        // password file override is even attempted; wifi_password_file_content
        // stays NULL, so network_init() below falls back to the flash-stored
        // PARAM_WIFI_PASSWORD config entry (its existing, already-supported
        // fallback for *pass == NULL -- see network.c, no new configuration
        // mechanism added here).
        DPRINTF("SD support disabled -- using flash-stored WiFi credentials only\r\n");
#endif

        cyw43_arch_deinit();

        network_init(true, NETWORK_CONNECTION_ASYNC, &wifi_password_file_content);
        absolute_time_t ABSOLUTE_TIME_INITIALIZED_VAR(reconnect_t, 0);
        absolute_time_t ABSOLUTE_TIME_INITIALIZED_VAR(second_t, 0);
        uint32_t time_to_connect_again = 1000; // 1 second
        bool network_ready = false;
        bool wifi_init = true;
        uint32_t wifi_timeout_sec = gemdrive_timeout_sec;

        // Wait until timeout
        while ((!network_ready) && (wifi_timeout_sec > 0) && (strlen(find_entry(PARAM_WIFI_SSID)->value) > 0))
        {
            *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = rand() % 0xFFFFFFFF;
#if PICO_CYW43_ARCH_POLL
            if (wifi_init)
            {
                cyw43_arch_poll();
            }
#endif

            // Check the cancel command
            if (active_command_id == GEMDRVEMUL_CANCEL)
            {
                DPRINTF("CANCEL command received!\n");
                wifi_timeout_sec = 0;
                write_random_token(memory_shared_address);
                active_command_id = 0xFFFF;
                break;
            }

            // Only display when changes status to avoid flooding the console
            ConnectionStatus previous_status = get_previous_connection_status();
            ConnectionStatus current_status = get_network_connection_status();
            if (current_status != previous_status)
            {
#if defined(_DEBUG) && (_DEBUG != 0)
                ConnectionData connection_data = {0};
                get_connection_data(&connection_data);
                DPRINTF("Status: %d - Prev: %d - SSID: %s - IPv4: %s - GW:%s - Mask:%s - MAC:%s\n",
                        current_status,
                        previous_status,
                        connection_data.ssid,
                        connection_data.ipv4_address,
                        print_ipv4(get_gateway()),
                        print_ipv4(get_netmask()),
                        print_mac(get_mac_address()));
#endif
                if ((current_status == GENERIC_ERROR) || (current_status == CONNECT_FAILED_ERROR) || (current_status == BADAUTH_ERROR))
                {
                    if (wifi_init)
                    {
                        network_terminate();
                        reconnect_t = make_timeout_time_ms(0);
                        time_to_connect_again = time_to_connect_again * 1.2;
                        wifi_init = false;
                        DPRINTF("Connection failed. Retrying in %d ms...\n", time_to_connect_again);
                    }
                }
            }
            network_ready = (current_status == CONNECTED_WIFI_IP);
            if (time_passed(&second_t, 1000) == 1)
            {
                DPRINTF("Timeout in seconds: %d\n", wifi_timeout_sec);
                wifi_timeout_sec--;
                second_t = make_timeout_time_ms(0);
            }

            // If SELECT button is pressed, launch the configurator
            if (gpio_get(SELECT_GPIO) != 0)
            {
                select_button_action(safe_config_reboot, write_config_only_once);
                // Write config only once to avoid hitting the flash too much
                write_config_only_once = false;
            }
            if ((!wifi_init) && (time_passed(&reconnect_t, time_to_connect_again) == 1))
            {
                network_init(true, NETWORK_CONNECTION_ASYNC, &wifi_password_file_content);
                reconnect_t = make_timeout_time_ms(0);
                wifi_init = true;
            }
        }
        if (wifi_timeout_sec <= 0)
        {
            // Just be sure to deinit the network stack
            network_terminate();
            DPRINTF("No wifi configured. Skipping network initialization.\n");
            sidetnfs_mark_network_skipped();
        }
        else
        {
            // We have network connection!
            // Fase 5B: local-only UDP PCB + udp_connect() toward the TNFS
            // server. Sends no payload, no TNFS command -- udp_connect() is
            // a local lwIP operation, nothing goes on the wire.
            sidetnfs_udp_connect_test();

            // Start the internal RTC
            rtc_init();

            ntp_server_host = find_entry(PARAM_RTC_NTP_SERVER_HOST)->value;
            ntp_server_port = atoi(find_entry(PARAM_RTC_NTP_SERVER_PORT)->value);

            DPRINTF("NTP server host: %s\n", ntp_server_host);
            DPRINTF("NTP server port: %d\n", ntp_server_port);

            char *utc_offset_entry = find_entry(PARAM_RTC_UTC_OFFSET)->value;
            if (strlen(utc_offset_entry) > 0)
            {
                // The offset can be in decimal format
                set_utc_offset_seconds((long)(atoi(utc_offset_entry) * 60 * 60));
            }
            DPRINTF("UTC offset: %ld\n", get_utc_offset_seconds());

            // Start the NTP client
            ntp_init();
            get_net_time()->ntp_server_found = false;

            bool dns_query_done = false;

            // Fase 8C: this loop previously had NO bound at all -- only
            // `while (get_rtc_time()->year == 0)`, with no timeout
            // decrement anywhere in its body. The "Timeout reached. RTC
            // not set." DPRINTF in the else-branch right after this loop
            // (unchanged below) already implies a timeout was intended,
            // but nothing ever produced one: a DNS failure or unreachable
            // NTP server would spin here forever, and since this all runs
            // *before* the main GEMDRVEMUL command-dispatch loop further
            // down, the Atari's PING (and the entire GEMDRIVE handshake)
            // would never be answered -- exactly the unbounded-network-wait
            // class of bug this phase requires eliminating. Bounded here
            // the same way the WiFi-connect wait above it already is:
            // reuses the same configured PARAM_GEMDRIVE_TIMEOUT_SEC value
            // (gemdrive_timeout_sec, default 45s), decremented once per
            // second. On expiry, falls through to the existing (already
            // correct) "RTC not set" else-branch below -- no other
            // behavior change.
            uint32_t ntp_timeout_sec = gemdrive_timeout_sec;
            absolute_time_t ABSOLUTE_TIME_INITIALIZED_VAR(ntp_second_t, 0);

            // Wait until the RTC is set by the NTP server, or the timeout expires
            while (get_rtc_time()->year == 0 && ntp_timeout_sec > 0)
            {
                if (time_passed(&ntp_second_t, 1000) == 1)
                {
                    ntp_timeout_sec--;
                    ntp_second_t = make_timeout_time_ms(0);
                }

#if PICO_CYW43_ARCH_POLL
                network_safe_poll();
#endif
                // Check the cancel command
                if (active_command_id == GEMDRVEMUL_CANCEL)
                {
                    DPRINTF("CANCEL command received!\n");
                    wifi_timeout_sec = 0;
                    write_random_token(memory_shared_address);
                    active_command_id = 0xFFFF;
                    break;
                }
                if ((get_net_time()->ntp_server_found) && dns_query_done)
                {
                    DPRINTF("NTP server found. Connecting to NTP server...\n");
                    get_net_time()->ntp_server_found = false;
                    set_internal_rtc();
                }
                // Get the IP address from the DNS server if the wifi is connected and no IP address is found yet
                if (!(dns_query_done))
                {
                    // Let's connect to ntp server
                    DPRINTF("Querying the DNS...\n");
                    err_t dns_ret = dns_gethostbyname(ntp_server_host, &get_net_time()->ntp_ipaddr, host_found_callback, get_net_time());
#if PICO_CYW43_ARCH_POLL
                    network_safe_poll();
#endif
                    if (dns_ret == ERR_ARG)
                    {
                        DPRINTF("Invalid DNS argument\n");
                    }
                    DPRINTF("DNS query done\n");
                    dns_query_done = true;
                }
                if (get_net_time()->ntp_error)
                {
                    DPRINTF("Error getting the NTP server IP address\n");
                    dns_query_done = false;
                    get_net_time()->ntp_error = false;
                    get_net_time()->ntp_server_found = false;
                }
                // If SELECT button is pressed, launch the configurator
                if (gpio_get(SELECT_GPIO) != 0)
                {
                    select_button_action(safe_config_reboot, write_config_only_once);
                    // Write config only once to avoid hitting the flash too much
                    write_config_only_once = false;
                }
            }
            if (get_rtc_time()->year != 0)
            {
                uint32_t gemdos_version = 0;
                get_shared_var(SHARED_VARIABLE_SVERSION, &gemdos_version, memory_shared_address);
                DPRINTF("Shared variable SVERSION: %x\n", gemdos_version);
                gemdos_version = gemdos_version & 0x0000FFFF;
                set_ikb_datetime_msg(memory_shared_address,
                        GEMDRVEMUL_RTC_DATETIME_BCD,
                        GEMDRVEMUL_RTC_Y2K_PATCH,
                        GEMDRVEMUL_RTC_DATETIME_MSDOS,
                        (int16_t)gemdos_version,
                        y2k_patch_enabled);
                        
                // If set then set the RTC and network status to 1, otherwise set it to 0
                *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RTC_STATUS)) = 0xFFFFFFFF;
                *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_NETWORK_STATUS)) = 0xFFFFFFFF;

                // Fase 5C: lazy, non-blocking TNFS MOUNT probe. This is the
                // last point in network setup where WiFi is still known to
                // be up (every other path below this falls back to
                // cyw43_arch_deinit()), and it runs before the main command
                // loop below ever sees a real Atari/GEMDRIVE handshake.
                sidetnfs_network_ok = true;
                sidetnfs_send_mount_probe();
            }
            else
            {
                DPRINTF("Timeout reached. RTC not set.\n");
                cyw43_arch_deinit();
                DPRINTF("No wifi configured. Skipping network initialization.\n");
                sidetnfs_mark_network_skipped();
            }
        }
    }
    else
    {
        // Just be sure to deinit the network stack
        cyw43_arch_deinit();
        DPRINTF("No wifi configured. Skipping network initialization.\n");
        sidetnfs_mark_network_skipped();
    }
#endif // SIDETNFS_CONFIG_DRIVE_ONLY

    DPRINTF("Waiting for commands...\n");

    while (true)
    {
        *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_RANDOM_TOKEN_SEED)) = rand() % 0xFFFFFFFF;
        tight_loop_contents();

// fully bypass the print variables when debug disabled
#if defined(_DEBUG) && (_DEBUG != 0)
        uint16_t old_command = active_command_id != 0xFFFF ? active_command_id : 0xFFFF;
#endif
        // Fase 5V/5X: log every genuinely-dispatched command_id (never on
        // idle 0xFFFF iterations) so a short, focused test (root listing +
        // ESC/refresh a few times + SELECT) can show whether an
        // unrecognized/unexpected command_id ever arrives instead of a
        // real GEMDRVEMUL_FSNEXT_CALL -- see report. Independent of
        // backend/listing routing so the same measurement can be
        // taken on the SD/FatFS baseline build (Fase 5X). Keep test
        // sessions short: this logs EVERY command, so a long session will
        // fill the 256-slot budget with unrelated Fopen/Fread/etc. noise
        // before the interesting part is even reached.
        //
        // Fase 7F-debugfix/7G: under SIDETNFS_DEBUG_FOCUS_FSEEK and/or
        // SIDETNFS_DEBUG_FOCUS_FDELETE, only log COMMAND_ENTER for the
        // small set of commands relevant to whichever focus mode(s) are
        // on -- the same "logging only, no control-flow change" contract
        // as SIDETNFS_DEBUG_FOCUS_FILE_IO. active_command_id itself is
        // read, never modified, by this block.
#if SIDETNFS_DEBUG_FOCUS_FSEEK || SIDETNFS_DEBUG_FOCUS_FDELETE || SIDETNFS_DEBUG_FOCUS_FRENAME || \
    SIDETNFS_DEBUG_FOCUS_DCREATE || SIDETNFS_DEBUG_FOCUS_DDELETE
        if ((SIDETNFS_DEBUG_FOCUS_FSEEK &&
             (active_command_id == GEMDRVEMUL_FOPEN_CALL || active_command_id == GEMDRVEMUL_READ_BUFF_CALL ||
              active_command_id == GEMDRVEMUL_FCLOSE_CALL || active_command_id == GEMDRVEMUL_FSEEK_CALL ||
              active_command_id == GEMDRVEMUL_FATTRIB_CALL || active_command_id == GEMDRVEMUL_FDATETIME_CALL ||
              active_command_id == GEMDRVEMUL_DGETPATH_CALL || active_command_id == GEMDRVEMUL_DSETPATH_CALL ||
              active_command_id == GEMDRVEMUL_PEXEC_CALL)) ||
            (SIDETNFS_DEBUG_FOCUS_FDELETE && active_command_id == GEMDRVEMUL_FDELETE_CALL) ||
            (SIDETNFS_DEBUG_FOCUS_FRENAME && active_command_id == GEMDRVEMUL_FRENAME_CALL) ||
            (SIDETNFS_DEBUG_FOCUS_DCREATE && active_command_id == GEMDRVEMUL_DCREATE_CALL) ||
            (SIDETNFS_DEBUG_FOCUS_DDELETE && active_command_id == GEMDRVEMUL_DDELETE_CALL))
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_COMMAND_ENTER, active_command_id, NULL, NULL, NULL, 0, 0, 0, 0);
        }
#else
        if (active_command_id != 0xFFFF)
        {
            sidetnfs_diag_log(SIDETNFS_DIAG_COMMAND_ENTER, active_command_id, NULL, NULL, NULL, 0, 0, 0, 0);
        }
#endif
        switch (active_command_id)
        {
        case GEMDRVEMUL_DEBUG:
        {
            uint32_t d3 = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            DPRINTF("DEBUG: %x\n", d3);
            payloadPtr += 2;
            uint32_t d4 = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            DPRINTF("DEBUG: %x\n", d4);
            payloadPtr += 2;
            uint32_t d5 = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            DPRINTF("DEBUG: %x\n", d5);
            payloadPtr += 2;
            uint8_t *payloadShowBytesPtr = (uint8_t *)payloadPtr;
            print_payload(payloadShowBytesPtr);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_CANCEL:
        {
            DPRINTF("CANCEL command received\n");
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SAVE_VECTORS:
        {
            DPRINTF("Saving vectors\n");
            uint32_t gemdos_trap_address_old = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
            payloadPtr += 2;
            uint32_t gemdos_trap_address_xbra = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            // Save the vectors needed for the floppy emulation
            DPRINTF("gemdos_trap_addres_xbra: %x\n", gemdos_trap_address_xbra);
            DPRINTF("gemdos_trap_address_old: %x\n", gemdos_trap_address_old);
            // DPRINTF("random token: %x\n", random_token);
            // Self modifying code to create the old and venerable XBRA structure
            *((volatile uint16_t *)(memory_firmware_code + gemdos_trap_address_xbra - ATARI_ROM4_START_ADDRESS)) = gemdos_trap_address_old & 0xFFFF;
            *((volatile uint16_t *)(memory_firmware_code + gemdos_trap_address_xbra - ATARI_ROM4_START_ADDRESS + 2)) = gemdos_trap_address_old >> 16;

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SAVE_XBIOS_VECTOR:
        {
            DPRINTF("Saving XBIOS vectors\n");
            xbios_trap_address_old = ((uint32_t)payloadPtr[0] << 16) | payloadPtr[1];
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_OLD_XBIOS_TRAP)) = xbios_trap_address_old & 0xFFFF;
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_OLD_XBIOS_TRAP + 2)) = xbios_trap_address_old >> 16;
            DPRINTF("xbios_trap_address_old: %x\n", xbios_trap_address_old);

            uint32_t gemdos_version = 0;
            get_shared_var(SHARED_VARIABLE_SVERSION, &gemdos_version, memory_shared_address);
            DPRINTF("Shared variable SVERSION: %x\n", gemdos_version);
            gemdos_version = gemdos_version & 0x0000FFFF;
            set_ikb_datetime_msg(memory_shared_address,
                        GEMDRVEMUL_RTC_DATETIME_BCD,
                        GEMDRVEMUL_RTC_Y2K_PATCH,
                        GEMDRVEMUL_RTC_DATETIME_MSDOS,
                        (int16_t)gemdos_version,
                        y2k_patch_enabled);

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_REENTRY_XBIOS_LOCK:
        {
            xbios_reentry_locked = true;
            DPRINTF("XBIOS Reentry locked\n");
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0xFFFF;
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_REENTRY_XBIOS_UNLOCK:
        {
            xbios_reentry_locked = false;
            DPRINTF("XBIOS Reentry unlocked\n");
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_RTC_XBIOS_REENTRY_TRAP)) = 0;
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_PING:
        {
            if (!hd_folder_ready)
            {
#if SIDETNFS_USE_TNFS_LISTING
                // Fase 8A: GEMDRIVE readiness must never depend on a
                // physical SD card under the TNFS backend -- the SD/FatFS
                // branch below (kept byte-identical for BACKEND_SD)
                // treated a failed/missing SD mount as "GEMDRIVE not
                // ready" (PING_STATUS 0x0), which made the whole TNFS
                // drive invisible to the Atari whenever no SD card was
                // present, even though TNFS itself needs no SD card at
                // all. SD mount is still attempted here, best-effort,
                // purely so hd_folder (used only by the SELECT-button
                // DEBUG.TXT snapshot -- see sidetnfs_diag_dump_on_select(),
                // which already no-ops safely on a NULL hd_folder) is
                // populated when a card happens to be present; a missing
                // or failed SD mount is not fatal here and never blocks
                // readiness. No FatFS file/directory operation beyond the
                // mount itself is ever attempted in this branch.
#if SIDETNFS_ENABLE_SD_SUPPORT
                if (!sd_init_driver())
                {
                    DPRINTF("INFO: No SD card detected -- TNFS-only, DEBUG.TXT snapshot unavailable\r\n");
                }
                else
                {
                    FRESULT fr = f_mount(&fs, "0:", 1);
                    if (fr == FR_OK)
                    {
                        hd_folder = find_entry(PARAM_GEMDRIVE_FOLDERS)->value;
                        sd_available = true;
                        DPRINTF("SD card also mounted (diagnostics only) in folder: %s\n", hd_folder);
                    }
                    else
                    {
                        DPRINTF("INFO: No SD filesystem mounted (%d) -- TNFS-only, DEBUG.TXT snapshot unavailable\r\n", fr);
                    }
                }
#else
                // Fase 8B: SIDETNFS_ENABLE_SD_SUPPORT==0 -- sd_init_driver()/
                // f_mount() are not even called, let alone reached. hd_folder
                // stays NULL for this whole boot (sidetnfs_diag_dump_on_select()
                // already no-ops safely on that, and SIDETNFS_ENABLE_DEBUG==0
                // disables it entirely anyway in this build).
#endif
                // Iterate over fdescriptors and close all files -- backend-
                // agnostic bookkeeping, needed regardless of whether SD
                // mounted above.
                close_all_files(&fdescriptors);
                cleanDTAHashTable();
#if SIDETNFS_CONFIG_DRIVE_ONLY
                // Fase 10B2: a config-drive search slot from a previous
                // Atari session (before a Pico reboot, or a stale ndta
                // otherwise) must never survive into this fresh boot.
                sidetnfs_config_drive_search_close_all();
#endif
                delete_all_files(&fdescriptors);
                DPRINTF("DTA table elements: %d\n", countDTA());
                DPRINTF("File descriptors: %d\n", count_fdesc(fdescriptors));
                dpath_string[0] = '\\'; // Set the root folder as default
                dpath_string[1] = '\0';
                hd_folder_ready = true;
                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_PING_STATUS)) = 0x1;
#else
                // Initialize SD card
                if (!sd_init_driver())
                {
                    DPRINTF("ERROR: Could not initialize SD card\r\n");
                    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_PING_STATUS)) = 0x0;
                }
                else
                {
                    // Mount drive
                    FRESULT fr; /* FatFs function common result code */
                    fr = f_mount(&fs, "0:", 1);
                    bool microsd_mounted = (fr == FR_OK);
                    if (!microsd_mounted)
                    {
                        DPRINTF("ERROR: Could not mount filesystem (%d)\r\n", fr);
                        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_PING_STATUS)) = 0x0;
                    }
                    else
                    {
                        hd_folder = find_entry(PARAM_GEMDRIVE_FOLDERS)->value;
                        sd_available = true;
                        DPRINTF("Emulating GEMDRIVE in folder: %s\n", hd_folder);
                        // Iterate over fdescriptors and close all files
                        close_all_files(&fdescriptors);
                        cleanDTAHashTable();
                        delete_all_files(&fdescriptors);
                        DPRINTF("DTA table elements: %d\n", countDTA());
                        DPRINTF("File descriptors: %d\n", count_fdesc(fdescriptors));
                        dpath_string[0] = '\\'; // Set the root folder as default
                        dpath_string[1] = '\0';
                        hd_folder_ready = true;

                        // Fase 5F/6F: the automatic dirty-flag-driven
                        // DEBUG.TXT write (sidetnfs_debug_file_service())
                        // that used to happen here on mount is permanently
                        // retired -- DEBUG.TXT is only ever written by the
                        // SELECT-button dump now (see report).

                        *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_PING_STATUS)) = 0x1;
                    }
                }
#endif
            }
            else if (sidetnfs_config_is_pending())
            {
                // Fase 9E: a SAVE_CONFIG happened since this GEMDRIVE
                // runtime was last activated (Pico cold boot, or a
                // previous reinit). hd_folder_ready is already true, so
                // the first-PING block above never runs again -- this is
                // the first PING of a NEW Atari boot/reset, the proven
                // moment to safely adopt the newly saved drive-list config
                // (see report: CMD_PING is sent once per Atari reset,
                // always before the GEMDOS/GEMDRIVE driver installs its
                // trap -- sidecart-gemdrive-atari/src/gemdrive.s,
                // test_ping/_ping_ready before save_vectors -- and never
                // again during normal use). Backend-agnostic: harmless
                // no-op for the TNFS-specific parts under the SD-only
                // backend, since no TNFS session/handles would exist there.
                close_all_files(&fdescriptors);
                cleanDTAHashTable();
#if SIDETNFS_CONFIG_DRIVE_ONLY
                // Fase 10B2: same as the first-PING cleanup above -- a
                // config-drive search slot from before this reinit must
                // never survive it.
                sidetnfs_config_drive_search_close_all();
#endif
                delete_all_files(&fdescriptors);
                DPRINTF("Fase 9E reinit -- DTA table elements: %d\n", countDTA());
                DPRINTF("Fase 9E reinit -- File descriptors: %d\n", count_fdesc(fdescriptors));
                dpath_string[0] = '\\'; // Set the root folder as default
                dpath_string[1] = '\0';

                // Closes old TNFS directory handles/fake-search state,
                // resets mount/session identity, reloads the persistent
                // config, and (re)selects the active server -- same init
                // path as Pico cold boot, never blocks.
                sidetnfs_probe_reinit_active_server(sidetnfs_network_ok);

                // Fase 9D/9E: select_gemdrive_drive_letter() above -- the
                // one shared implementation, using the freshly reloaded
                // active server. The Atari driver reads
                // SHARED_VARIABLE_DRIVE_LETTER right after this PING
                // succeeds (gemdrive.s), before installing its GEMDOS trap,
                // so it always picks up whatever is published here.
                char reinit_drive_letter = select_gemdrive_drive_letter();
                uint32_t reinit_drive_letter_num = (uint8_t)toupper(reinit_drive_letter);
                uint32_t reinit_drive_number = reinit_drive_letter_num - 65;
                set_shared_var(SHARED_VARIABLE_DRIVE_LETTER, reinit_drive_letter_num, memory_shared_address);
                set_shared_var(SHARED_VARIABLE_DRIVE_NUMBER, reinit_drive_number, memory_shared_address);

                // Only now, after the new config/drive/server have all
                // been adopted, clear the pending flag -- a later
                // SAVE_CONFIG sets it again for the next reset. Never
                // cleared on a path that skipped adoption.
                sidetnfs_config_clear_pending();

                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_PING_STATUS)) = 0x1;
            }
            DPRINTF("PING received. Answering with: %d\n", *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_PING_STATUS)));
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SHOW_VECTOR_CALL:
        {
            uint16_t trap_call = (uint16_t)payloadPtr[0];
            bool isBlacklisted = false;
            for (int i = 0; i < sizeof(BLACKLISTED_GEMDOS_CALLS); i++)
            {
                if (trap_call == BLACKLISTED_GEMDOS_CALLS[i])
                {
                    isBlacklisted = true; // Found the call in the blacklist
                    break;
                }
            }
            // if (!isBlacklisted)
            // {
            // If the call is not blacklisted, print its information
            DPRINTF("GEMDOS CALL: %s (%x)\n", GEMDOS_CALLS[trap_call], trap_call);
            // }
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SET_SHARED_VAR:
        {
            // Shared variables
            uint32_t shared_variable_index = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d3 register
            payloadPtr += 2;                                                                  // Skip two words
            uint32_t shared_variable_value = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d4 register
            set_shared_var(shared_variable_index, shared_variable_value, memory_shared_address);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_DGETDRV_CALL:
        {
            // Get the drive letter
            uint16_t dgetdrive_value = (uint16_t)payloadPtr[0];
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_REENTRY_LOCK:
        {
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_REENTRY_TRAP)) = 0xFFFF;
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_REENTRY_UNLOCK:
        {
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_REENTRY_TRAP)) = 0x0;
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SIDETNFS_GET_CONFIG_INFO:
        {
            // Fase 9C: minimal, read-only probe. No request payload, no
            // SD/FatFS/WiFi/TNFS/flash access, no handle or session
            // changes -- five fixed 32-bit fields, protocol version 2. See
            // docs/sidetnfs-config-protocol.md.
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_CONFIG_VERSION, SIDETNFS_CONFIG_PROTOCOL_VERSION);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_CONFIG_MAX_DRIVES, sidetnfs_config_get_max_drives());
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_COUNT, sidetnfs_config_get_drive_count());
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_CONFIG_DRIVE_LETTER, sidetnfs_config_get_config_drive_letter());
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_CONFIG_STATUS, SIDETNFS_CONFIG_STATUS_OK);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SIDETNFS_GET_DRIVE:
        {
            // Fase 9C: read-only lookup of one ordinary drive record from
            // the RAM drive list. No SD/WiFi/TNFS/flash access, no
            // handle/session changes. Request: one uint32_t index (same
            // d3-register convention as GEMDRVEMUL_DFREE_CALL/
            // shared-variable reads above). See
            // docs/sidetnfs-config-protocol.md for the exact wire layout.
            uint32_t drive_index = GET_PAYLOAD_PARAM32(payloadPtr);

            sidetnfs_drive_config_t drive;
            sidetnfs_config_status_t result = sidetnfs_config_get_drive((uint8_t)drive_index, &drive);

            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_STATUS, (uint32_t)result);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_USED, (uint16_t)drive.used);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_LETTER, (uint16_t)drive.drive_letter);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_TYPE, (uint16_t)drive.type);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_TRANSPORT, (uint16_t)drive.transport);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_PORT, drive.port);

            // Pico->Atari string transfer: byte-copy + CHANGE_ENDIANESS_BLOCK16
            // in place -- same pattern populate_dta() uses. Fase 9C-R:
            // an earlier revision of this code removed the
            // CHANGE_ENDIANESS_BLOCK16 step based on a host-only
            // simulation that modeled Atari reads as plain, unmodified
            // byte fetches of Pico RAM. Hardware testing disproved that:
            // without the swap, "RetroLoft" came back as "eRtLofo" --
            // exact pairwise byte swap. The real ROM3 bus evidently
            // reorders byte-granularity reads of the 16-bit-wide shared
            // memory (68000 UDS/LDS byte-lane selection against the
            // RP2040's little-endian storage), which the host simulation
            // did not model -- WRITE_WORD/WRITE_AND_SWAP_LONGWORD fields
            // are unaffected because those are single word/long bus
            // cycles, not sequences of independent byte reads. This swap
            // is required and must not be removed without a corrected
            // hardware-validated model. See docs/sidetnfs-config-protocol.md.
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_DRIVE_NICKNAME), drive.nickname, SIDETNFS_NICKNAME_LEN);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_DRIVE_NICKNAME, SIDETNFS_NICKNAME_LEN);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_DRIVE_HOST), drive.host, SIDETNFS_HOST_LEN);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_DRIVE_HOST, SIDETNFS_HOST_LEN);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_DRIVE_MOUNT_PATH), drive.mount_path, SIDETNFS_MOUNTPATH_LEN);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_DRIVE_MOUNT_PATH, SIDETNFS_MOUNTPATH_LEN);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH), drive.sd_path, SIDETNFS_SDPATH_LEN);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_DRIVE_SD_PATH, SIDETNFS_SDPATH_LEN);

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SIDETNFS_SET_DRIVE:
        {
            // Fase 9C: RAM-only write of one ordinary drive record. No
            // flash access. Request payload mirrors GET_DRIVE's response
            // field order (minus status): index, then the same
            // used/drive_letter/type/transport/port/nickname/host/
            // mount_path/sd_path fields, read sequentially from
            // payloadPtr -- the same word-by-word/COPY_AND_CHANGE_ENDIANESS_BLOCK16
            // convention GEMDRVEMUL_FOPEN_CALL/DSETPATH_CALL already use to
            // read Atari->Pico string arguments. See
            // docs/sidetnfs-config-protocol.md.
            uint32_t drive_index = GET_PAYLOAD_PARAM32(payloadPtr);
            payloadPtr += 2;

            sidetnfs_drive_config_t drive;
            memset(&drive, 0, sizeof(drive));

            drive.used = (uint8_t)GET_PAYLOAD_PARAM16(payloadPtr);
            payloadPtr += 1;
            drive.drive_letter = (uint8_t)GET_PAYLOAD_PARAM16(payloadPtr);
            payloadPtr += 1;
            drive.type = (uint8_t)GET_PAYLOAD_PARAM16(payloadPtr);
            payloadPtr += 1;
            drive.transport = (uint8_t)GET_PAYLOAD_PARAM16(payloadPtr);
            payloadPtr += 1;
            drive.port = GET_PAYLOAD_PARAM16(payloadPtr);
            payloadPtr += 1;

            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, drive.nickname, SIDETNFS_NICKNAME_LEN);
            payloadPtr += SIDETNFS_NICKNAME_LEN / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, drive.host, SIDETNFS_HOST_LEN);
            payloadPtr += SIDETNFS_HOST_LEN / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, drive.mount_path, SIDETNFS_MOUNTPATH_LEN);
            payloadPtr += SIDETNFS_MOUNTPATH_LEN / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, drive.sd_path, SIDETNFS_SDPATH_LEN);
            payloadPtr += SIDETNFS_SDPATH_LEN / 2;

            sidetnfs_config_status_t result = sidetnfs_config_set_drive((uint8_t)drive_index, &drive);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_STATUS, (uint32_t)result);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SIDETNFS_DELETE_DRIVE:
        {
            // Fase 9C: RAM-only clear of one ordinary drive record. Can
            // never touch the config drive (it has no index in this
            // array). Request: one uint32_t index. Response: status only.
            uint32_t drive_index = GET_PAYLOAD_PARAM32(payloadPtr);
            sidetnfs_config_status_t result = sidetnfs_config_delete_drive((uint8_t)drive_index);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_STATUS, (uint32_t)result);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SIDETNFS_SET_CONFIG_DRIVE:
        {
            // Fase 9C: RAM-only change of the config drive letter. No
            // flash access. Request: one uint32_t ASCII letter. Response:
            // status only.
            uint32_t new_letter = GET_PAYLOAD_PARAM32(payloadPtr);
            sidetnfs_config_status_t result = sidetnfs_config_set_config_drive_letter((uint8_t)new_letter);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_STATUS, (uint32_t)result);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SIDETNFS_SAVE_CONFIG:
        {
            // Fase 9C: the only command in this protocol that ever
            // touches flash. Validates the full RAM drive list, then
            // erases+programs exactly one 4KB flash sector, reads it back
            // via XIP, and verifies magic/version/CRC before reporting
            // success. Does not touch the active TNFS session/hd_folder/
            // open handles/DTAs -- a reboot is required before any future
            // runtime code uses the saved list. Request: none. Response:
            // status only.
            sidetnfs_config_status_t result = sidetnfs_config_save();
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_DRIVE_STATUS, (uint32_t)result);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SIDETNFS_GET_NETWORK_CONFIG:
        {
            // --- Fase 11C, real Fase 11A getter restored on aligned offsets ---
            // Root cause (hardware-confirmed via Test 1/1A/1B/1C, see
            // report): GEMDRVEMUL_SIDETNFS_NETWORK_STATUS used to be
            // 0x4472, only 2-byte aligned, because the preceding 198-byte
            // drive record's own size is not a multiple of 4.
            // WRITE_AND_SWAP_LONGWORD's raw `*(volatile uint32_t*)` store
            // there was an unaligned 32-bit access, which Cortex-M0+
            // cannot perform in hardware -- HardFault. Fixed structurally
            // in gemdrvemul.h: GEMDRVEMUL_SIDETNFS_NETWORK now rounds up
            // to the next 4-byte boundary (SIDETNFS_NETWORK_ALIGN4()),
            // moving STATUS to 0x4474 (4-byte aligned) and every
            // subsequent field with it; _Static_assert()s there guard the
            // alignment/bounds guarantees at compile time. Plain
            // WRITE_AND_SWAP_LONGWORD is safe again.
            //
            // Hardware-validated on the new offsets by the aligned Test 2
            // build (fixed literals, preserved below in #if 0) together
            // with the user's updated AtariConfig client. This is the
            // real Fase 11A implementation (verbatim, promoted back to
            // active from the #if 0 block further below) -- read-only:
            // sidetnfs_netconfig_get() only ever calls find_entry(), a
            // pure RAM lookup against the existing configData.
            sidetnfs_network_config_t netcfg;
            sidetnfs_netconfig_get(&netcfg);

            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS,
                                     (uint32_t)SIDETNFS_NETCONFIG_STATUS_OK);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE, netcfg.auth_mode);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP, netcfg.use_dhcp);

            // Pico->Atari string transfer: same hardware-proven byte-copy +
            // CHANGE_ENDIANESS_BLOCK16 pattern GET_DRIVE uses above -- see
            // that block's comment for the full rationale. Never remove
            // this swap based on host-only testing.
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_SSID), netcfg.ssid, MAX_SSID_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_SSID, MAX_SSID_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD), netcfg.password, MAX_PASSWORD_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD, MAX_PASSWORD_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY), netcfg.country, SIDETNFS_NET_COUNTRY_LEN);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY, SIDETNFS_NET_COUNTRY_LEN);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS), netcfg.ip_address, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK), netcfg.netmask, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY), netcfg.gateway, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_DNS), netcfg.primary_dns, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_DNS, IPV4_ADDRESS_LENGTH);

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#if 0
            // --- Fase 11C, aligned Test 2 build (superseded by real getter above) ---
            // Same fixed-literal synthetic response as Test 2 (proves the
            // response region/offsets/write helpers on the NEW aligned
            // addresses) -- no sidetnfs_netconfig_get(), no find_entry(),
            // no other config/network/flash function, no payloadPtr.
            // Hardware-validated together with the user's updated
            // AtariConfig client; superseded once the real getter above
            // was restored.
            char test2_ssid[MAX_SSID_LENGTH];
            char test2_password[MAX_PASSWORD_LENGTH];
            char test2_country[SIDETNFS_NET_COUNTRY_LEN];
            char test2_ip_address[IPV4_ADDRESS_LENGTH];
            char test2_netmask[IPV4_ADDRESS_LENGTH];
            char test2_gateway[IPV4_ADDRESS_LENGTH];
            char test2_primary_dns[IPV4_ADDRESS_LENGTH];

            memset(test2_ssid, 0, sizeof(test2_ssid));
            strncpy(test2_ssid, "TEST_WIFI", sizeof(test2_ssid) - 1);
            memset(test2_password, 0, sizeof(test2_password));
            strncpy(test2_password, "test_password", sizeof(test2_password) - 1);
            memset(test2_country, 0, sizeof(test2_country));
            strncpy(test2_country, "NL", sizeof(test2_country) - 1);
            memset(test2_ip_address, 0, sizeof(test2_ip_address));
            strncpy(test2_ip_address, "192.168.1.100", sizeof(test2_ip_address) - 1);
            memset(test2_netmask, 0, sizeof(test2_netmask));
            strncpy(test2_netmask, "255.255.255.0", sizeof(test2_netmask) - 1);
            memset(test2_gateway, 0, sizeof(test2_gateway));
            strncpy(test2_gateway, "192.168.1.1", sizeof(test2_gateway) - 1);
            memset(test2_primary_dns, 0, sizeof(test2_primary_dns));
            strncpy(test2_primary_dns, "1.1.1.1", sizeof(test2_primary_dns) - 1);

            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS,
                                     (uint32_t)SIDETNFS_NETCONFIG_STATUS_OK);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE, 4); // WPA2_AES_PSK
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP, 1);

            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_SSID), test2_ssid, MAX_SSID_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_SSID, MAX_SSID_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD), test2_password, MAX_PASSWORD_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD, MAX_PASSWORD_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY), test2_country, SIDETNFS_NET_COUNTRY_LEN);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY, SIDETNFS_NET_COUNTRY_LEN);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS), test2_ip_address, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK), test2_netmask, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY), test2_gateway, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_DNS), test2_primary_dns, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_DNS, IPV4_ADDRESS_LENGTH);

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif
#if 0
            // --- Fase 11C, Test 1C isolation build (superseded by aligned Test 2 above) ---
            uint32_t status = (uint32_t)SIDETNFS_NETCONFIG_STATUS_NOT_STAGED;
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS, (uint16_t)(status >> 16));
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS + 2, (uint16_t)(status & 0xFFFF));
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif
#if 0
            // --- Fase 11C, Test 1B isolation build (superseded by Test 1C above) ---
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif
#if 0
            // --- Fase 11C, Test 1A isolation build (superseded by Test 1B above) ---
            active_command_id = 0xFFFF;
            break;
#endif
#if 0
            // --- Fase 11C, Test 2 isolation build (superseded by Test 1A above) ---
            // Full synthetic response: proves the response region,
            // offsets, write helpers, and Atari-side readback in
            // isolation. Every field below is a local fixed literal --
            // never sidetnfs_netconfig_get(), find_entry(), or any other
            // config/network/flash function, and payloadPtr is never
            // touched. Same WRITE_WORD/WRITE_AND_SWAP_LONGWORD/memcpy+
            // CHANGE_ENDIANESS_BLOCK16 primitives the real handler uses
            // (see the disabled #if 0 block below) -- never a raw struct
            // copy onto ROM3. Each local buffer is memset(0) across its
            // FULL declared field length before strncpy() -- guarantees
            // no leftover bytes from any earlier use of that stack slot
            // and provable NUL-termination within bounds, independent of
            // strncpy's own (also-safe) short-source padding behavior.
            // No dynamic allocation. Test 1's no-op version and the real
            // Fase 11A implementation are both preserved verbatim below
            // (#if 0), not deleted.
            char test2_ssid[MAX_SSID_LENGTH];
            char test2_password[MAX_PASSWORD_LENGTH];
            char test2_country[SIDETNFS_NET_COUNTRY_LEN];
            char test2_ip_address[IPV4_ADDRESS_LENGTH];
            char test2_netmask[IPV4_ADDRESS_LENGTH];
            char test2_gateway[IPV4_ADDRESS_LENGTH];
            char test2_primary_dns[IPV4_ADDRESS_LENGTH];

            memset(test2_ssid, 0, sizeof(test2_ssid));
            strncpy(test2_ssid, "TEST_WIFI", sizeof(test2_ssid) - 1);
            memset(test2_password, 0, sizeof(test2_password));
            strncpy(test2_password, "test_password", sizeof(test2_password) - 1);
            memset(test2_country, 0, sizeof(test2_country));
            strncpy(test2_country, "NL", sizeof(test2_country) - 1);
            memset(test2_ip_address, 0, sizeof(test2_ip_address));
            strncpy(test2_ip_address, "192.168.1.100", sizeof(test2_ip_address) - 1);
            memset(test2_netmask, 0, sizeof(test2_netmask));
            strncpy(test2_netmask, "255.255.255.0", sizeof(test2_netmask) - 1);
            memset(test2_gateway, 0, sizeof(test2_gateway));
            strncpy(test2_gateway, "192.168.1.1", sizeof(test2_gateway) - 1);
            memset(test2_primary_dns, 0, sizeof(test2_primary_dns));
            strncpy(test2_primary_dns, "1.1.1.1", sizeof(test2_primary_dns) - 1);

            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS,
                                     (uint32_t)SIDETNFS_NETCONFIG_STATUS_OK);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE, 4); // WPA2_AES_PSK, see network.c get_auth_pico_code() mapping (3,4,5)
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP, 1);

            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_SSID), test2_ssid, MAX_SSID_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_SSID, MAX_SSID_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD), test2_password, MAX_PASSWORD_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD, MAX_PASSWORD_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY), test2_country, SIDETNFS_NET_COUNTRY_LEN);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY, SIDETNFS_NET_COUNTRY_LEN);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS), test2_ip_address, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK), test2_netmask, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY), test2_gateway, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_DNS), test2_primary_dns, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_DNS, IPV4_ADDRESS_LENGTH);

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#if 0
            // --- Fase 11C, Test 1 isolation build (superseded by Test 2 above) ---
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS,
                                     (uint32_t)SIDETNFS_NETCONFIG_STATUS_NOT_STAGED);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif
#if 0
            // Fase 11A: read-only. No request payload, no SD/WiFi/flash
            // I/O -- sidetnfs_netconfig_get() only ever calls find_entry(),
            // a pure RAM lookup against the existing configData. See
            // docs/sidetnfs-config-protocol.md.
            sidetnfs_network_config_t netcfg;
            sidetnfs_netconfig_get(&netcfg);

            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS,
                                     (uint32_t)SIDETNFS_NETCONFIG_STATUS_OK);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_AUTH_MODE, netcfg.auth_mode);
            WRITE_WORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_USE_DHCP, netcfg.use_dhcp);

            // Pico->Atari string transfer: same hardware-proven byte-copy +
            // CHANGE_ENDIANESS_BLOCK16 pattern GET_DRIVE uses above -- see
            // that block's comment for the full rationale. Never remove
            // this swap based on host-only testing.
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_SSID), netcfg.ssid, MAX_SSID_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_SSID, MAX_SSID_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD), netcfg.password, MAX_PASSWORD_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_PASSWORD, MAX_PASSWORD_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY), netcfg.country, SIDETNFS_NET_COUNTRY_LEN);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_COUNTRY, SIDETNFS_NET_COUNTRY_LEN);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS), netcfg.ip_address, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_IP_ADDRESS, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK), netcfg.netmask, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_NETMASK, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY), netcfg.gateway, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_GATEWAY, IPV4_ADDRESS_LENGTH);
            memcpy((void *)(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_DNS), netcfg.primary_dns, IPV4_ADDRESS_LENGTH);
            CHANGE_ENDIANESS_BLOCK16(memory_shared_address + GEMDRVEMUL_SIDETNFS_NETWORK_DNS, IPV4_ADDRESS_LENGTH);

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif // Fase 11A original implementation
#endif // Fase 11C Test 2 isolation build wrapper
        }
        case GEMDRVEMUL_SIDETNFS_SET_NETWORK_CONFIG:
        {
            // Fase 11A: RAM-only staging write. Request payload mirrors
            // GET_NETWORK_CONFIG's response field order (minus STATUS),
            // read via payloadPtr the same word-by-word/
            // COPY_AND_CHANGE_ENDIANESS_BLOCK16 convention SET_DRIVE uses
            // above. Validates first (sidetnfs_netconfig_stage() ->
            // sidetnfs_netconfig_validate()); on any failure the previous
            // staging copy is left completely untouched, and configData/
            // flash/the active network connection are never touched
            // either way. See docs/sidetnfs-config-protocol.md.
            sidetnfs_network_config_t netcfg;
            memset(&netcfg, 0, sizeof(netcfg));

            netcfg.auth_mode = GET_PAYLOAD_PARAM16(payloadPtr);
            payloadPtr += 1;
            netcfg.use_dhcp = GET_PAYLOAD_PARAM16(payloadPtr);
            payloadPtr += 1;

            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, netcfg.ssid, MAX_SSID_LENGTH);
            payloadPtr += MAX_SSID_LENGTH / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, netcfg.password, MAX_PASSWORD_LENGTH);
            payloadPtr += MAX_PASSWORD_LENGTH / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, netcfg.country, SIDETNFS_NET_COUNTRY_LEN);
            payloadPtr += SIDETNFS_NET_COUNTRY_LEN / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, netcfg.ip_address, IPV4_ADDRESS_LENGTH);
            payloadPtr += IPV4_ADDRESS_LENGTH / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, netcfg.netmask, IPV4_ADDRESS_LENGTH);
            payloadPtr += IPV4_ADDRESS_LENGTH / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, netcfg.gateway, IPV4_ADDRESS_LENGTH);
            payloadPtr += IPV4_ADDRESS_LENGTH / 2;
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, netcfg.primary_dns, IPV4_ADDRESS_LENGTH);
            payloadPtr += IPV4_ADDRESS_LENGTH / 2;

            sidetnfs_netconfig_status_t netconfig_result = sidetnfs_netconfig_stage(&netcfg);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS, (uint32_t)netconfig_result);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SIDETNFS_SAVE_NETWORK_CONFIG:
        {
            // Fase 11A: the only command in this block that ever touches
            // flash. Re-validates the staged copy, updates the nine
            // existing PARAM_WIFI_* configData entries, writes the
            // existing 8KB CONFIG_FLASH sector (via write_all_entries(),
            // fixed this same phase for 256-byte flash_range_program()
            // alignment -- see config.c), and only reports OK after an
            // exact nine-field XIP readback. Never touches the active
            // WiFi connection -- see sidetnfs_netconfig_save(). Request:
            // none. Response: status only.
            sidetnfs_netconfig_status_t netconfig_result = sidetnfs_netconfig_save();
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_SIDETNFS_NETWORK_STATUS, (uint32_t)netconfig_result);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_DFREE_CALL:
        {
            uint32_t dfree_unit = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            // Check the free space
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B: fixed 256 KiB virtual capacity, reported entirely
            // full (read-only drive -- see sidetnfs_config_drive_get_disk_info()).
            ScFsDiskInfo disk_info;
            sidetnfs_config_drive_get_disk_info(&disk_info.total_clusters, &disk_info.free_clusters,
                                                  &disk_info.bytes_per_sector, &disk_info.sectors_per_cluster);
            bool disk_info_ok = true;
#else
            // Fase 7N: tnfsd 24.0522.1 has no TNFS command for free/total
            // disk space -- no SIZE/FREE/SIZEBYTES/FREEBYTES-equivalent
            // opcode, and no statvfs()/statfs() call anywhere in its
            // source (same server-source verification approach as Fase
            // 7Lb's CHMOD finding and Fase 7M's UTIME/SETTIME finding). No
            // TNFS packet is ever sent for Dfree. This is deliberately
            // NOT reported as an unsupported-operation error the way
            // Fattrib set/Fdatime set are -- GEMDOS Dfree has no sensible
            // "unsupported" GEMDOS error code, and software calling Dfree
            // generally expects *some* plausible disk-size answer rather
            // than a hard failure. Fixed synthetic values are returned
            // instead: ~256 MiB total (65535 clusters * 8 sectors/cluster
            // * 512 bytes/sector), ~128 MiB free (32768 of those
            // clusters).
            sidetnfs_diag_log(SIDETNFS_DIAG_DFREE_SYNTHETIC, 0, NULL, NULL, NULL, 0, 0, 0, 0);
            ScFsDiskInfo disk_info;
            disk_info.free_clusters = 32768;
            disk_info.total_clusters = 65535;
            disk_info.bytes_per_sector = 512;
            disk_info.sectors_per_cluster = 8;
            bool disk_info_ok = true;
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
            ScFsDiskInfo disk_info;
            bool disk_info_ok = scfs_get_disk_info(hd_folder, &disk_info);
#endif
            if (!disk_info_ok)
            {
                *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DFREE_STATUS)) = GEMDOS_ERROR;
            }
            else
            {
                DPRINTF("Total clusters: %d, free clusters: %d, bytes per sector: %d, sectors per cluster: %d\n", disk_info.total_clusters, disk_info.free_clusters, disk_info.bytes_per_sector, disk_info.sectors_per_cluster);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_DFREE_STRUCT, disk_info.free_clusters);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_DFREE_STRUCT + 4, disk_info.total_clusters);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_DFREE_STRUCT + 8, disk_info.bytes_per_sector);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_DFREE_STRUCT + 12, disk_info.sectors_per_cluster);
                *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_DFREE_STATUS)) = GEMDOS_EOK;
            }
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_DGETPATH_CALL:
        {
            uint16_t dpath_drive = payloadPtr[0]; // d3 register

            DPRINTF("Dpath drive: %x\n", dpath_drive);
            DPRINTF("Dpath string: %s\n", dpath_string);

            char tmp_path[MAX_FOLDER_LENGTH] = {0};
            memccpy(tmp_path, dpath_string, 0, MAX_FOLDER_LENGTH);
            forward_2_backslash(tmp_path);

            // Remove the backslash at the end
            DPRINTF("Dpath backslash string: %s\n", tmp_path);
            if (tmp_path[strlen(tmp_path) - 1] == '\\')
            {
                tmp_path[strlen(tmp_path) - 1] = '\0';
            }

            DPRINTF("Dpath backslash string (no last backslash: %s\n", tmp_path);

#if SIDETNFS_USE_TNFS_LISTING
            // Fase 7E: informational only -- Dgetpath's logic itself is
            // unchanged (dpath_string was already forward-slash-form
            // internally, and this conversion back to backslash for GEMDOS
            // already existed); this just makes what's actually returned
            // visible in DEBUG.TXT now that Dsetpath can correctly update
            // dpath_string for a real TNFS subdirectory.
            sidetnfs_diag_log(SIDETNFS_DIAG_DGETPATH_RETURN, 0, tmp_path, NULL, NULL, 0, 0, 0, 0);
#endif
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(tmp_path, memory_shared_address + GEMDRVEMUL_DEFAULT_PATH, MAX_FOLDER_LENGTH);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_DSETPATH_CALL:
        {
#if SIDETNFS_USE_TNFS_LISTING
            // Fase 8A: the old "still unconditionally hard-SD" note here is
            // stale -- see the SIDETNFS_USE_TNFS_LISTING/#else split further
            // down in this case (Fase 8A), which now never calls
            // scfs_directory_exists()/touches hd_folder under this backend.
            sidetnfs_diag_log(SIDETNFS_DIAG_DSETPATH_ENTER, 0, NULL, NULL, NULL, 0, 0, 0, 0);
#endif
            payloadPtr += 6; // Skip six words
            // Obtain the fname string and keep it in memory
            char dpath_tmp[MAX_FOLDER_LENGTH] = {};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, dpath_tmp, MAX_FOLDER_LENGTH);
            DPRINTF("Default path string: %s\n", dpath_tmp);
#if SIDETNFS_USE_TNFS_LISTING
            sidetnfs_diag_log(SIDETNFS_DIAG_DSETPATH_PATH_RAW, 0, dpath_tmp, NULL, NULL, 0, 0, 0, 0);
#endif
            // Check if the directory exists
            if (dpath_tmp[0] == drive_letter)
            {
                DPRINTF("Drive letter found: %c. Removing it.\n", drive_letter);
                // Remove the drive letter and the colon from the path
                memmove(dpath_tmp, dpath_tmp + 2, strlen(dpath_tmp));
            }

            DPRINTF("Dpath string: %s\n", dpath_string);
            DPRINTF("Dpath tmp: %s\n", dpath_tmp);

            // Check if the path is relative or absolute
            if ((dpath_tmp[0] != '\\') && (dpath_tmp[0] != '/'))
            {
                // Concatenate the path with the existing dpath_string
                char tmp_path_concat[MAX_FOLDER_LENGTH] = {0};
                snprintf(tmp_path_concat, sizeof(tmp_path_concat), "%s/%s", dpath_string, dpath_tmp);
                DPRINTF("Concatenated path: %s\n", tmp_path_concat);
                strcpy(dpath_tmp, tmp_path_concat);
                DPRINTF("Dpath tmp: %s\n", dpath_tmp);
            }
            else
            {
                DPRINTF("Do not concatenate the path\n");
            }
            back_2_forwardslash(dpath_tmp);
            // Remove duplicated forward slashes
            remove_dup_slashes(dpath_tmp);

#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B: root-only drive -- Dsetpath only succeeds for the
            // root itself ("" or "/"); any subdirectory reference is a
            // clean GEMDOS_EPTHNF, since this backend has no directories.
            char dsetpath_config_path[MAX_FOLDER_LENGTH];
            snprintf(dsetpath_config_path, sizeof(dsetpath_config_path), "%s", dpath_tmp);
            size_t dsetpath_config_path_len = strlen(dsetpath_config_path);
            if (dsetpath_config_path_len > 1 && dsetpath_config_path[dsetpath_config_path_len - 1] == '/')
            {
                dsetpath_config_path[dsetpath_config_path_len - 1] = '\0';
            }
            bool dsetpath_exists = (dsetpath_config_path[0] == '\0' || strcmp(dsetpath_config_path, "/") == 0);
#else
            // Fase 8A: the old unconditional scfs_directory_exists(tmp_path)
            // "SD_CHECK" (comparison-only, never affecting dsetpath_exists
            // under this backend) touched the physical SD card via FatFS
            // even when the TNFS backend is active -- a real cross-backend
            // isolation violation, and, worse, tmp_path was built from
            // hd_folder which is NULL whenever the SD card never mounted,
            // making the snprintf() below it undefined behavior. Removed
            // entirely; TNFS mode now never builds an SD path or touches
            // hd_folder here at all. See report.
            //
            // dpath_tmp is already the fully-resolved,
            // forward-slash, drive-letter-stripped, cwd-relative path (e.g.
            // "/CONFIG") -- the exact same shape Fsfirst/Fopen already read
            // out of dpath_string (see get_tnfs_relative_pathname() and
            // FSFIRST_CALL's own relative-path concatenation below). A
            // trailing slash is stripped only for this existence check (a
            // local copy -- dpath_tmp/dpath_string themselves are left
            // untouched, exactly as before) to match how
            // gemdrive_backend_fsfirst() already normalizes tnfs_path.
            char dsetpath_tnfs_path[MAX_FOLDER_LENGTH];
            snprintf(dsetpath_tnfs_path, sizeof(dsetpath_tnfs_path), "%s", dpath_tmp);
            size_t dsetpath_tnfs_path_len = strlen(dsetpath_tnfs_path);
            if (dsetpath_tnfs_path_len > 1 && dsetpath_tnfs_path[dsetpath_tnfs_path_len - 1] == '/')
            {
                dsetpath_tnfs_path[dsetpath_tnfs_path_len - 1] = '\0';
            }
            sidetnfs_diag_log(SIDETNFS_DIAG_DSETPATH_TNFS_PATH, 0, dsetpath_tnfs_path, NULL, NULL, 0, 0, 0, 0);
            uint8_t dsetpath_tnfs_rc = 0;
            bool dsetpath_exists = sidetnfs_tnfs_directory_exists(dsetpath_tnfs_path, &dsetpath_tnfs_rc);
            sidetnfs_diag_log(SIDETNFS_DIAG_DSETPATH_TNFS_EXISTS_RC, 0, dsetpath_tnfs_path, NULL, NULL, 0, 0,
                               dsetpath_tnfs_rc, 0);
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
            char tmp_path[MAX_FOLDER_LENGTH] = {0};
            // Concatenate the path with the hd_folder
            snprintf(tmp_path, sizeof(tmp_path), "%s/%s", hd_folder, dpath_tmp);
            remove_dup_slashes(tmp_path);
            bool dsetpath_exists = scfs_directory_exists(tmp_path);
#endif
            if (dsetpath_exists)
            {
                DPRINTF("Directory exists: %s\n", dpath_tmp);
                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_SET_DPATH_STATUS)) = GEMDOS_EOK;
            }
            else
            {
                DPRINTF("Directory does not exist: %s\n", dpath_tmp);
                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_SET_DPATH_STATUS)) = GEMDOS_EPTHNF;
            }
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B-afronding: a failed Dsetpath (any non-root reference,
            // since this drive has no subdirectories) must never mutate
            // dpath_string -- the existing CWD (always root) stays exactly
            // as it was. The unconditional strcpy() below is the
            // TNFS/SD-backend behavior, unchanged and out of scope here.
            if (dsetpath_exists)
            {
                strcpy(dpath_string, dpath_tmp);
                DPRINTF("The new default path is: %s\n", dpath_string);
            }
#else
            // Copy dpath_tmp to dpath_string
            strcpy(dpath_string, dpath_tmp);
            DPRINTF("The new default path is: %s\n", dpath_string);
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#if SIDETNFS_USE_TNFS_LISTING
            sidetnfs_diag_log(SIDETNFS_DIAG_DSETPATH_TNFS_CWD_SET, 0, dpath_string, NULL, NULL, 0, 0,
                               (uint8_t)(dsetpath_exists ? GEMDOS_EOK : GEMDOS_EPTHNF), 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_DSETPATH_RETURN, 0, dpath_string, NULL, NULL, 0, 0,
                               (uint8_t)(dsetpath_exists ? GEMDOS_EOK : GEMDOS_EPTHNF), 0);
#endif
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_DCREATE_CALL:
        {
            // Obtain the pathname string and keep it in memory
            // concatenated with the local harddisk folder and the default path (if any)
            payloadPtr += 6; // Skip six words
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B: read-only drive -- Dcreate is always denied, never
            // even parses the target path (this drive has no subdirectories).
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DCREATE_STATUS)) = GEMDOS_EACCDN;
#else
            // Fase 7I: real TNFS mkdir. No pre-check that the directory
            // already exists (no directory listing, no stat) -- the
            // server's own mkdir() rc alone determines the result, so a
            // create can never be reported as successful unless the
            // server actually performed it.
            char tnfs_dcreate_raw_path[MAX_FOLDER_LENGTH] = {0};
            char tnfs_dcreate_path[MAX_FOLDER_LENGTH] = {0};
            get_tnfs_relative_pathname(tnfs_dcreate_path, tnfs_dcreate_raw_path, NULL);
            DPRINTF("TNFS folder to create: %s\n", tnfs_dcreate_path);

            sidetnfs_diag_log(SIDETNFS_DIAG_DCREATE_ENTER, 0, NULL, NULL, NULL, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_DCREATE_RAW_PATH, 0, tnfs_dcreate_raw_path, NULL, NULL, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_DCREATE_TNFS_PATH, 0, tnfs_dcreate_path, NULL, NULL, 0, 0, 0, 0);

            uint8_t tnfs_dcreate_rc = 0xFFu;
            SidetnfsDirCreateResult tnfs_dcreate_result =
                sidetnfs_tnfs_directory_create(tnfs_dcreate_path, &tnfs_dcreate_rc);
            sidetnfs_note_tnfs_dcreate(tnfs_dcreate_path, tnfs_dcreate_rc,
                                        tnfs_dcreate_result == SIDETNFS_DIR_CREATE_OK);

            uint16_t tnfs_dcreate_status;
            switch (tnfs_dcreate_result)
            {
            case SIDETNFS_DIR_CREATE_OK:
                tnfs_dcreate_status = GEMDOS_EOK;
                break;
            case SIDETNFS_DIR_CREATE_PATH_NOT_FOUND:
                tnfs_dcreate_status = GEMDOS_EPTHNF;
                break;
            case SIDETNFS_DIR_CREATE_ACCESS_DENIED:
                tnfs_dcreate_status = GEMDOS_EACCDN;
                break;
            case SIDETNFS_DIR_CREATE_ERROR:
            default:
                tnfs_dcreate_status = GEMDOS_EINTRN;
                break;
            }
            sidetnfs_diag_log(SIDETNFS_DIAG_DCREATE_RETURN, 0, tnfs_dcreate_path, NULL, NULL, 0, 0,
                               (uint8_t)tnfs_dcreate_status, 0);
            // Fase 7I: matches the existing (SD-established) plain
            // uint16_t write for this specific status field -- see report
            // for why this differs from the uint32_t+SWAP_LONGWORD pattern
            // used by Fdelete/Frename/Fseek's status fields.
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DCREATE_STATUS)) = tnfs_dcreate_status;
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
            char tmp_pathname[MAX_FOLDER_LENGTH] = {0};
            scfs_get_local_full_pathname(tmp_pathname);
            DPRINTF("Folder to create: %s\n", tmp_pathname);

            // Check if the folder exists. If not, return an error
            if (scfs_directory_exists(tmp_pathname) != FR_OK)
            {
                DPRINTF("ERROR: Folder does not exist\n");
                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DCREATE_STATUS)) = GEMDOS_EPTHNF;
            }
            else
            {
                // Create the folder
                fr = f_mkdir(tmp_pathname);
                if (fr != FR_OK)
                {
                    DPRINTF("ERROR: Could not create folder (%d)\r\n", fr);
                    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DCREATE_STATUS)) = GEMDOS_EACCDN;
                }
                else
                {
                    DPRINTF("Folder created\n");
                    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DCREATE_STATUS)) = GEMDOS_EOK;
                }
            }
#endif
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_DDELETE_CALL:
        {
            // Obtain the pathname string and keep it in memory
            // concatenated with the local harddisk folder and the default path (if any)
            payloadPtr += 6; // Skip six words
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B: read-only drive -- Ddelete is always denied, never
            // even parses the target path (this drive has no subdirectories).
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DDELETE_STATUS)) = GEMDOS_EACCDN;
#else
            // Fase 7J: real TNFS rmdir. No pre-check via directory
            // listing/enumeration to see whether the directory is empty --
            // the server must bear that responsibility atomically, and a
            // refusal (e.g. not empty) is always mapped to a GEMDOS error,
            // never treated as success. Never a recursive delete, never a
            // fallback to UNLINK.
            char tnfs_ddelete_raw_path[MAX_FOLDER_LENGTH] = {0};
            char tnfs_ddelete_path[MAX_FOLDER_LENGTH] = {0};
            get_tnfs_relative_pathname(tnfs_ddelete_path, tnfs_ddelete_raw_path, NULL);
            DPRINTF("TNFS folder to delete: %s\n", tnfs_ddelete_path);

            sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_ENTER, 0, NULL, NULL, NULL, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_RAW_PATH, 0, tnfs_ddelete_raw_path, NULL, NULL, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_TNFS_PATH, 0, tnfs_ddelete_path, NULL, NULL, 0, 0, 0, 0);

            uint16_t tnfs_ddelete_status;
            // Fase 7J-correctie2: root removal must still never succeed, and
            // is still denied locally before ever contacting the server.
            // The cwd-equals-target denial that used to sit alongside it is
            // REMOVED -- hardware evidence (see report) showed GEM Desktop
            // routinely Dsetpath()'s into a folder to inspect it, then
            // Ddelete's that same folder by its full path, which is a
            // perfectly normal, expected sequence. It must be let through to
            // TNFS RMDIR exactly like the SD/FatFS route always has (that
            // route never denied this case at all).
            bool tnfs_ddelete_is_cwd = strcmp(tnfs_ddelete_path, dpath_string) == 0;
            bool tnfs_ddelete_is_root = strcmp(tnfs_ddelete_path, "/") == 0;
            if (tnfs_ddelete_is_root)
            {
                sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_CWD_CHECK, 0, tnfs_ddelete_path, NULL, NULL, 0, 0, 1, 0);
                tnfs_ddelete_status = GEMDOS_EACCDN;
                sidetnfs_note_tnfs_ddelete(tnfs_ddelete_path, 0xFFu, false);
                sidetnfs_note_tnfs_ddelete_diag(false, true, false, "ROOT");
            }
            else
            {
                sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_CWD_CHECK, 0, tnfs_ddelete_path, NULL, NULL, 0, 0, 0, 0);

                if (tnfs_ddelete_is_cwd)
                {
                    sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_CWD_MATCH_ALLOWED, 0, tnfs_ddelete_path, NULL, NULL, 0,
                                       0, 0, 0);
                    sidetnfs_note_tnfs_ddelete_cwd(true, false, dpath_string, NULL);
                }

                // Fase 7J-correctie: close every active TNFS DTA-registry
                // search handle whose path exactly matches the target
                // directory BEFORE ever sending RMDIR -- a still-open
                // OPENDIRX handle on this exact directory (e.g. Desktop's
                // own enumeration of it, not yet closed) can make the
                // server refuse RMDIR. Unlike the old post-success release,
                // this close is confirmed: if it fails, RMDIR is never
                // attempted.
                sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_DTA_PRECHECK, 0, tnfs_ddelete_path, NULL, NULL, 0, 0, 0, 0);
                uint16_t tnfs_ddelete_dta_matches = 0;
                uint8_t tnfs_ddelete_dta_close_rc = 0xFFu;
                bool tnfs_ddelete_dta_ok = sidetnfs_tnfs_dta_close_by_path(
                    tnfs_ddelete_path, &tnfs_ddelete_dta_matches, &tnfs_ddelete_dta_close_rc);
                sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_DTA_RELEASE_BEFORE, 0, tnfs_ddelete_path, NULL, NULL,
                                    tnfs_ddelete_dta_matches, 0, tnfs_ddelete_dta_close_rc,
                                    tnfs_ddelete_dta_ok ? 1 : 0);

                if (!tnfs_ddelete_dta_ok)
                {
                    // At least one matching search handle could not be
                    // confirmed closed -- the server may still consider the
                    // directory open, so RMDIR must not be attempted.
                    tnfs_ddelete_status = GEMDOS_EINTRN;
                    sidetnfs_note_tnfs_ddelete(tnfs_ddelete_path, tnfs_ddelete_dta_close_rc, false);
                    sidetnfs_note_tnfs_ddelete_diag(false, false, false, "DTA_CLOSE");
                }
                else
                {
                    sidetnfs_note_tnfs_ddelete_diag(false, false, true, NULL);
                    sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_RMDIR_SENT, 0, tnfs_ddelete_path, NULL, NULL, 0, 0, 0, 0);
                    uint8_t tnfs_ddelete_rc = 0xFFu;
                    SidetnfsDirDeleteResult tnfs_ddelete_result =
                        sidetnfs_tnfs_directory_delete(tnfs_ddelete_path, &tnfs_ddelete_rc);
                    sidetnfs_note_tnfs_ddelete(tnfs_ddelete_path, tnfs_ddelete_rc,
                                                tnfs_ddelete_result == SIDETNFS_DIR_DELETE_OK);

                    switch (tnfs_ddelete_result)
                    {
                    case SIDETNFS_DIR_DELETE_OK:
                        tnfs_ddelete_status = GEMDOS_EOK;
                        sidetnfs_note_tnfs_ddelete_diag(false, false, false, "OK");
                        sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_RMDIR_OK, 0, tnfs_ddelete_path, NULL, NULL, 0, 0, 0,
                                           0);
                        if (tnfs_ddelete_is_cwd)
                        {
                            // Fase 7J-correctie2: only ever rewrite the TNFS
                            // CWD after a *confirmed* TNFS_OK RMDIR of the
                            // directory that was the CWD -- never on any
                            // other outcome. Same canonicalization the path
                            // resolver already produced for tnfs_ddelete_path
                            // itself (forward-slash, no trailing slash), so
                            // no re-normalization is needed beyond the
                            // parent-path computation itself.
                            char tnfs_ddelete_new_cwd[MAX_FOLDER_LENGTH];
                            tnfs_ddelete_parent_path(tnfs_ddelete_path, tnfs_ddelete_new_cwd,
                                                      sizeof(tnfs_ddelete_new_cwd));
                            strcpy(dpath_string, tnfs_ddelete_new_cwd);
                            sidetnfs_note_tnfs_ddelete_cwd(false, true, NULL, dpath_string);
                            sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_CWD_PARENT_UPDATE, 0, dpath_string, NULL, NULL,
                                               0, 0, 0, 0);
                        }
                        break;
                    case SIDETNFS_DIR_DELETE_PATH_NOT_FOUND:
                        tnfs_ddelete_status = GEMDOS_EPTHNF;
                        sidetnfs_note_tnfs_ddelete_diag(false, false, false, "RMDIR_PTHNF");
                        break;
                    case SIDETNFS_DIR_DELETE_ACCESS_DENIED:
                        tnfs_ddelete_status = GEMDOS_EACCDN;
                        sidetnfs_note_tnfs_ddelete_diag(false, false, false, "RMDIR_ACCDN");
                        break;
                    case SIDETNFS_DIR_DELETE_ERROR:
                    default:
                        tnfs_ddelete_status = GEMDOS_EINTRN;
                        sidetnfs_note_tnfs_ddelete_diag(false, false, false, "RMDIR_ERROR");
                        break;
                    }
                }
            }
            sidetnfs_diag_log(SIDETNFS_DIAG_DDELETE_RETURN, 0, tnfs_ddelete_path, NULL, NULL, 0, 0,
                               (uint8_t)tnfs_ddelete_status, 0);
            // Fase 7J: matches the existing (SD-established) plain
            // uint16_t write for this status field -- same convention as
            // Dcreate's GEMDRVEMUL_DCREATE_STATUS (see Fase 7I report).
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DDELETE_STATUS)) = tnfs_ddelete_status;
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
            char tmp_pathname[MAX_FOLDER_LENGTH] = {0};
            scfs_get_local_full_pathname(tmp_pathname);
            DPRINTF("Folder to delete: %s\n", tmp_pathname);

            // Check if the folder exists. If not, return an error
            if (scfs_directory_exists(tmp_pathname) == 0)
            {
                DPRINTF("ERROR: Folder does not exist\n");
                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DDELETE_STATUS)) = GEMDOS_EPTHNF;
            }
            else
            {
                // Delete the folder
                fr = f_unlink(tmp_pathname);
                if (fr != FR_OK)
                {
                    DPRINTF("ERROR: Could not delete folder (%d)\r\n", fr);
                    if (fr == FR_DENIED)
                    {
                        DPRINTF("ERROR: Folder is not empty\n");
                    }
                    else if (fr == FR_NO_PATH)
                    {
                        DPRINTF("ERROR: Folder does not exist\n");
                    }
                    else
                    {
                        DPRINTF("ERROR: Internal error: %d\n", fr);
                    }
                }
                else
                {
                    DPRINTF("Folder deleted\n");
                }
                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_DDELETE_STATUS)) = scfs_fresult_to_gemdos_error(fr);
            }
#endif
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_FSETDTA_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B2: the config drive allocates its search slot later,
            // in Fsfirst (sidetnfs_config_drive_search_start()) -- Fsetdta
            // itself does nothing here. In particular, never calls
            // insertDTA(ndta, data, NULL, NULL, 0): that FatFS path
            // dereferences dj->pat with dj == NULL, which this backend must
            // never exercise. Still a normal, fully-confirmed command.
            (void)ndta;
#else
            bool ndta_exists = lookupDTA(ndta);
            if (ndta_exists)
            {
                // We don't release the DTA if it already exists. Wait for FsFirst to do it
                DPRINTF("DTA at %x already exists.\n", ndta);
            }
            else
            {
                DTA data = {"filename", 0, 0, 0, 0, 0, 0, 0, 0, "filename"};
                insertDTA(ndta, data, NULL, NULL, 0);
                DPRINTF("Added ndta: %x.\n", ndta);
            }
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_DTA_EXIST_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            bool ndta_exists = gemdrive_backend_dta_exists(ndta);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_DTA_EXIST, (ndta_exists ? ndta : 0));
            sidetnfs_diag_log(SIDETNFS_DIAG_DTA_EXIST_RETURN, ndta, NULL, NULL, NULL, 0, 0, (uint8_t)ndta_exists, 0);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_DTA_RELEASE_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
            DPRINTF("Releasing DTA: %x\n", ndta);
            sidetnfs_diag_log(SIDETNFS_DIAG_DTA_RELEASE_ENTER, ndta, NULL, NULL, NULL, 0, 0, 0, 0);
            uint32_t release_count = gemdrive_backend_dta_release(ndta, memory_shared_address);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_DTA_RELEASE, release_count);
            sidetnfs_diag_log(SIDETNFS_DIAG_DTA_RELEASE_RETURN, ndta, NULL, NULL, NULL, 0, 0, 0,
                               (uint8_t)release_count);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_FSFIRST_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];  // d3 register
            payloadPtr += 2;                                                  // Skip two words
            uint32_t attribs = payloadPtr[0];                                 // d4 register
            payloadPtr += 2;                                                  // Skip two words
            uint32_t fspec = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d5 register
            payloadPtr += 2;                                                  // Skip two words
            char attribs_str[7] = "";
            char internal_path[MAX_FOLDER_LENGTH * 2] = {0};
            char pattern[MAX_FOLDER_LENGTH] = {0};
            char fspec_string[MAX_FOLDER_LENGTH] = {0};
            char tmp_string[MAX_FOLDER_LENGTH] = {0};
            char path_forwardslash[MAX_FOLDER_LENGTH] = {0};
            // swap_string_endiannes((char *)payloadPtr, tmp_string);
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(payloadPtr, tmp_string, MAX_FOLDER_LENGTH);
            DPRINTF("Fspec string: %s\n", tmp_string);
            back_2_forwardslash(tmp_string);
            DPRINTF("Fspec string backslash: %s\n", tmp_string);

            if (tmp_string[1] == ':')
            {
                // If the path has the drive letter, jump two positions
                // and ignore the dpath_string
                snprintf(tmp_string, MAX_FOLDER_LENGTH, "%s", tmp_string + 2);
                DPRINTF("New path_filename: %s\n", tmp_string);
            }
            bool fsfirst_path_was_relative = false;
            if (tmp_string[0] == '/')
            {
                DPRINTF("Root folder found. Ignoring default path.\n");
                strcpy(fspec_string, tmp_string);
            }
            else
            {
                DPRINTF("Need to concatenate the default path: %s\n", dpath_string);
#if SIDETNFS_USE_TNFS_LISTING
                // Fase 7E: Fsfirst had no dedicated path-resolve
                // diagnostics of its own before this phase (unlike Fopen's
                // FOPEN_RAW_PATH/FOPEN_TNFS_PATH) -- these three cover the
                // same raw-input/cwd/resolved-output shape for the
                // relative-fspec case.
                fsfirst_path_was_relative = true;
                sidetnfs_diag_log(SIDETNFS_DIAG_PATH_RESOLVE_INPUT, ndta, tmp_string, NULL, NULL, 0, 0, 0, 0);
                sidetnfs_diag_log(SIDETNFS_DIAG_PATH_RESOLVE_CWD, ndta, dpath_string, NULL, NULL, 0, 0, 0, 0);
#endif
                snprintf(fspec_string, sizeof(fspec_string), "%s/%s", dpath_string, tmp_string);
                DPRINTF("Full fspec string: %s\n", fspec_string);
            }

            // Remove duplicated forward slashes
            remove_dup_slashes(fspec_string);
            get_attribs_st_str(attribs_str, attribs);
            seach_path_2_st(fspec_string, internal_path, path_forwardslash, pattern);

            back_2_forwardslash(path_forwardslash);
#if SIDETNFS_USE_TNFS_LISTING
            if (fsfirst_path_was_relative)
            {
                sidetnfs_diag_log(SIDETNFS_DIAG_PATH_RESOLVE_OUTPUT, ndta, path_forwardslash, NULL, NULL, 0, 0, 0, 0);
            }
#endif

            // Testing if the FSfirst changes the default path or not
            // DPRINTF("Old dpath string: %s, new dpath string: %s\n", dpath_string, path_forwardslash);
            // strcpy(dpath_string, path_forwardslash);

            // Remove all the trailing spaces in the pattern
            remove_trailing_spaces(pattern);

            DPRINTF("Fsfirst ndta: %x, attribs: %s, fspec: %x, fspec string: %s\n", ndta, attribs_str, fspec, fspec_string);
            DPRINTF("Fsfirst Full internal path: %s, filename pattern: %s[%d]\n", internal_path, pattern, strlen(pattern));

            // Fase 5X: fires identically on the SD baseline build too.
            sidetnfs_diag_log(SIDETNFS_DIAG_FSFIRST_ENTER, ndta, path_forwardslash, pattern, NULL, 0, 0, 0,
                               (uint8_t)attribs);

            if (!(attribs & FS_ST_LABEL))
            {
                attribs |= FS_ST_ARCH;
            }

            gemdrive_backend_fsfirst(ndta, path_forwardslash, internal_path, pattern, attribs, memory_shared_address,
                                      sidetnfs_network_ok);

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_FSNEXT_CALL:
        {
            uint32_t ndta = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d3 register
            DPRINTF("Fsnext ndta: %x\n", ndta);

            gemdrive_backend_fsnext(ndta, memory_shared_address);

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_FOPEN_CALL:
        {
            uint16_t fopen_mode = payloadPtr[0]; // d3 register
            payloadPtr += 6;                     // Skip six words
            // Obtain the fname string and keep it in memory
            // concatenated path and filename
            char tmp_filepath[MAX_FOLDER_LENGTH] = {0};
#if SIDETNFS_USE_TNFS_LISTING
            // Fase 7D-debug: capture the raw (untouched) payload path and
            // the pre-slash-normalization intermediate, purely for
            // diagnosis -- the resulting tmp_filepath is unchanged.
            char fopen_raw_path[MAX_FOLDER_LENGTH] = {0};
            char fopen_internal_path[MAX_FOLDER_LENGTH] = {0};
            get_tnfs_relative_pathname(tmp_filepath, fopen_raw_path, fopen_internal_path);
            sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_MODE, 0, NULL, NULL, NULL, fopen_mode, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_RAW_PATH, 0, fopen_raw_path, NULL, NULL, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_INTERNAL_PATH, 0, fopen_internal_path, NULL, NULL, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_FOPEN_TNFS_PATH, 0, tmp_filepath, NULL, NULL, 0, 0, 0, 0);
#else
            get_local_full_pathname(tmp_filepath);
#endif
            DPRINTF("Opening file: %s with mode: %x\n", tmp_filepath, fopen_mode);
            gemdrive_backend_fopen(fopen_mode, tmp_filepath, memory_shared_address);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_FCLOSE_CALL:
        {
            uint16_t fclose_fd = payloadPtr[0]; // d3 register
            DPRINTF("Closing file with fd: %x\n", fclose_fd);
            gemdrive_backend_fclose(fclose_fd, memory_shared_address);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_FCREATE_CALL:
        {
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B: read-only drive -- Fcreate is always denied, never
            // even parses the target path (nothing to create/truncate).
            payloadPtr += 6; // Skip six words (fcreate_mode/path, same shape as below)
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCREATE_HANDLE)) = GEMDOS_EACCDN;
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#else
            // Fase 7K: real TNFS create+truncate (Fcreate always creates a
            // new file or truncates an existing one -- see
            // sidetnfs_tnfs_file_create()). fcreate_mode is the Atari
            // attribute byte for the new file (hidden/system/etc.) -- not
            // acted on yet, same "MISSING ATTRIBUTE MODIFICATION" gap the
            // SD/FatFS route below already has.
            fcreate_mode = payloadPtr[0]; // d3 register
            payloadPtr += 6;              // Skip six words
            char tnfs_fcreate_raw_path[MAX_FOLDER_LENGTH] = {0};
            char tnfs_fcreate_path[MAX_FOLDER_LENGTH] = {0};
            get_tnfs_relative_pathname(tnfs_fcreate_path, tnfs_fcreate_raw_path, NULL);
            DPRINTF("TNFS file to create: %s (attr %x)\n", tnfs_fcreate_path, fcreate_mode);

            uint8_t tnfs_fcreate_handle = 0;
            SidetnfsFileOpenResult tnfs_fcreate_result = sidetnfs_tnfs_file_create(tnfs_fcreate_path, &tnfs_fcreate_handle);
            if (tnfs_fcreate_result != SIDETNFS_FILE_OPEN_OK)
            {
                DPRINTF("ERROR: Could not create TNFS file %s (result %d)\n", tnfs_fcreate_path, (int)tnfs_fcreate_result);
                int32_t tnfs_fcreate_gemdos_rc =
                    tnfs_fcreate_result == SIDETNFS_FILE_OPEN_NOT_FOUND ? GEMDOS_EPTHNF : GEMDOS_EINTRN;
                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCREATE_HANDLE)) = tnfs_fcreate_gemdos_rc;
            }
            else
            {
                uint16_t tnfs_fcreate_fd_counter = get_first_available_fd(fdescriptors);
                FileDescriptors *newFDescriptor = malloc(sizeof(FileDescriptors));
                if (newFDescriptor == NULL)
                {
                    DPRINTF("Memory allocation failed for new FileDescriptors\n");
                    // Best-effort -- don't leak the TNFS-side handle just
                    // because the local tracking entry couldn't be allocated.
                    sidetnfs_tnfs_file_close(tnfs_fcreate_fd_counter, tnfs_fcreate_handle);
                    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCREATE_HANDLE)) = GEMDOS_EINTRN;
                }
                else
                {
                    add_tnfs_file(&fdescriptors, newFDescriptor, tnfs_fcreate_path, tnfs_fcreate_handle,
                                  tnfs_fcreate_fd_counter, true);
                    DPRINTF("TNFS file created with file descriptor: %d\n", tnfs_fcreate_fd_counter);
                    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCREATE_HANDLE)) = tnfs_fcreate_fd_counter;
                }
            }
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
            fcreate_mode = payloadPtr[0]; // d3 register
            payloadPtr += 6;              // Skip six words
            // Obtain the fname string and keep it in memory
            // concatenated path and filename
            char tmp_filepath[MAX_FOLDER_LENGTH] = {0};
            get_local_full_pathname(tmp_filepath);
            DPRINTF("Creating file: %s\n with mode: %x", tmp_filepath, fcreate_mode);

            // CREATE ALWAYS MODE
            BYTE fatfs_create_mode = FA_READ | FA_WRITE | FA_CREATE_ALWAYS;
            DPRINTF("FatFs create mode: %x\n", fatfs_create_mode);

            // Open the file with FatFs
            FIL file_object;
            fr = f_open(&file_object, tmp_filepath, fatfs_create_mode);
            if (fr != FR_OK)
            {
                DPRINTF("ERROR: Could not create file (%d)\r\n", fr);
                // *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FCREATE_HANDLE)) = SWAP_LONGWORD(GEMDOS_EPTHNF);
                *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCREATE_HANDLE)) = GEMDOS_EPTHNF;
            }
            else
            {
                // Add the file to the list of open files
                int fd_counter = get_first_available_fd(fdescriptors);
                DPRINTF("File created with file descriptor: %d\n", fd_counter);
                FileDescriptors *newFDescriptor = malloc(sizeof(FileDescriptors));
                if (newFDescriptor == NULL)
                {
                    DPRINTF("Memory allocation failed for new FileDescriptors\n");
                    DPRINTF("ERROR: Could not add file to the list of open files\n");
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FCREATE_HANDLE, GEMDOS_EINTRN);
                }
                else
                {
                    add_file(&fdescriptors, newFDescriptor, tmp_filepath, file_object, fd_counter);

                    // MISSING ATTRIBUTE MODIFICATION

                    // Return the file descriptor
                    *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_FCREATE_HANDLE)) = fd_counter;
                }
            }

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif
        }
        case GEMDRVEMUL_FDELETE_CALL:
        {
            payloadPtr += 6; // Skip six words
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B: read-only drive -- Fdelete is always denied, never
            // even parses the target path (nothing to delete).
            *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FDELETE_STATUS)) = SWAP_LONGWORD(GEMDOS_EACCDN);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#else
            // Fase 7G: real TNFS file delete. Directories are never
            // deleted here -- see sidetnfs_tnfs_file_delete()'s comment:
            // relies on the server's own unlink() refusing a directory
            // (standard POSIX semantics) rather than a separate
            // TNFS_CMD_STAT type-check, and any refusal is always mapped
            // to a GEMDOS error, never success.
            char tnfs_delete_raw_path[MAX_FOLDER_LENGTH] = {0};
            char tnfs_delete_path[MAX_FOLDER_LENGTH] = {0};
            get_tnfs_relative_pathname(tnfs_delete_path, tnfs_delete_raw_path, NULL);
            sidetnfs_diag_log(SIDETNFS_DIAG_FDELETE_ENTER, 0, NULL, NULL, NULL, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_FDELETE_RAW_PATH, 0, tnfs_delete_raw_path, NULL, NULL, 0, 0, 0, 0);
            sidetnfs_diag_log(SIDETNFS_DIAG_FDELETE_TNFS_PATH, 0, tnfs_delete_path, NULL, NULL, 0, 0, 0, 0);

            // Fase 7G: mirrors the SD route's own "if it's open, close it
            // first" step below, using the existing TNFS close helper (no
            // change to Fclose itself) -- avoids leaving a dangling TNFS
            // handle/local descriptor behind for a file we're about to
            // delete.
            FileDescriptors *tnfs_delete_file = get_file_by_fpath(fdescriptors, tnfs_delete_path);
            if (tnfs_delete_file != NULL && tnfs_delete_file->backend == GEMDRIVE_FILE_BACKEND_TNFS)
            {
                DPRINTF("File is open. Closing it first\n");
                sidetnfs_tnfs_file_close(tnfs_delete_file->fd, tnfs_delete_file->tnfs_handle);
                delete_file_by_fdesc(&fdescriptors, tnfs_delete_file->fd);
            }

            uint8_t tnfs_delete_rc = 0xFFu;
            SidetnfsFileDeleteResult tnfs_delete_result = sidetnfs_tnfs_file_delete(tnfs_delete_path, &tnfs_delete_rc);
            sidetnfs_note_tnfs_fdelete(tnfs_delete_path, tnfs_delete_rc, tnfs_delete_result == SIDETNFS_FILE_DELETE_OK);

            uint32_t tnfs_delete_status;
            switch (tnfs_delete_result)
            {
            case SIDETNFS_FILE_DELETE_OK:
                tnfs_delete_status = GEMDOS_EOK;
                break;
            case SIDETNFS_FILE_DELETE_NOT_FOUND:
                // TNFS's single ENOENT doesn't distinguish "file not
                // found" from "path not found" -- same limitation already
                // noted for Fopen (Fase 7D) and Dsetpath (Fase 7E).
                tnfs_delete_status = GEMDOS_EFILNF;
                break;
            case SIDETNFS_FILE_DELETE_ACCESS_DENIED:
                // Covers both a real access-denied and the server
                // refusing to unlink a directory (EISDIR) -- either way,
                // deny, never silently treat as deleted.
                tnfs_delete_status = GEMDOS_EACCDN;
                break;
            case SIDETNFS_FILE_DELETE_ERROR:
            default:
                tnfs_delete_status = GEMDOS_EINTRN;
                break;
            }
            sidetnfs_diag_log(SIDETNFS_DIAG_FDELETE_RETURN, 0, tnfs_delete_path, NULL, NULL, 0, 0,
                               (uint8_t)tnfs_delete_status, 0);
            *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FDELETE_STATUS)) =
                SWAP_LONGWORD(tnfs_delete_status);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
            // Obtain the fname string and keep it in memory
            // concatenated path and filename
            char tmp_filepath[MAX_FOLDER_LENGTH] = {0};
            get_local_full_pathname(tmp_filepath);
            uint32_t status = GEMDOS_EOK;
            // Check first if the file is open. If so, close it first.
            FileDescriptors *file = get_file_by_fpath(fdescriptors, tmp_filepath);
            if (file != NULL)
            {
                DPRINTF("File is open. Closing it first\n");
                fr = f_close(&file->fobject);
                if (fr != FR_OK)
                {
                    DPRINTF("ERROR: Could not close file (%d)\r\n", fr);
                    status = GEMDOS_EINTRN;
                }
                // In both cases, remove the file from the list of open files
                delete_file_by_fdesc(&fdescriptors, file->fd);
            }
            // If the file was open and it was not possible to close it, return an error
            if (status == GEMDOS_EOK)
            {
                DPRINTF("Deleting file: %s\n", tmp_filepath);
                // Delete the file
                fr = f_unlink(tmp_filepath);
                if (fr != FR_OK)
                {
                    DPRINTF("ERROR: Could not delete file (%d)\r\n", fr);
                    if (fr == FR_DENIED)
                    {
                        DPRINTF("ERROR: Not enough permissions to delete file\n");
                        status = GEMDOS_EACCDN;
                    }
                    else if (fr == FR_NO_PATH)
                    {
                        DPRINTF("ERROR: Folder does not exist\n");
                        status = GEMDOS_EPTHNF;
                    }
                    else if (fr == FR_NO_FILE)
                    {
                        DPRINTF("ERROR: File does not exist\n");
                        // status = GEMDOS_EFILNF;
                        status = GEMDOS_EOK;
                    }
                    else
                    {
                        DPRINTF("ERROR: Internal error\n");
                        status = GEMDOS_EINTRN;
                    }
                }
                else
                {
                    DPRINTF("File deleted\n");
                    status = GEMDOS_EOK;
                }
            }
            *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FDELETE_STATUS)) = SWAP_LONGWORD(status);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
#endif
        }
        case GEMDRVEMUL_FSEEK_CALL:
        {
            uint16_t fseek_fd = payloadPtr[0];                                       // d3 register
            payloadPtr += 2;                                                         // Skip two words
            uint32_t fseek_offset = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d4 register
            payloadPtr += 2;                                                         // Skip two words
            uint16_t fseek_mode = payloadPtr[0];                                     // d5 register
            DPRINTF("Fseek in the file with fd: %x, offset: %x, mode: %x\n", fseek_fd, fseek_offset, fseek_mode);
            // Obtain the file descriptor
            FileDescriptors *file = get_file_by_fdesc(fdescriptors, fseek_fd);
            if (file == NULL)
            {
                DPRINTF("ERROR: File descriptor not found\n");
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FSEEK_STATUS, GEMDOS_EIHNDL);
            }
#if SIDETNFS_USE_TNFS_LISTING
            else if (file->backend == GEMDRIVE_FILE_BACKEND_TNFS)
            {
                // Fase 7F: real TNFS Fseek. SEEK_SET/SEEK_CUR are resolved
                // to an absolute target offset locally -- exactly mirroring
                // the SD/RAM route's own arithmetic below, including its
                // existing (here intentionally unchanged) clamp checks --
                // then sent as an absolute TNFS SEEK_SET so the server-side
                // read cursor actually moves there too (GEMDRVEMUL_READ_BUFF_CALL's
                // TNFS path is unchanged this phase and just keeps reading
                // sequentially from wherever the server cursor is). Only
                // SEEK_END asks the server directly, since only it knows
                // the file size.
                sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_ENTER, fseek_fd, NULL, NULL, NULL, fseek_mode, 0, 0, 0);
                sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_HANDLE, fseek_fd, NULL, NULL, NULL, file->tnfs_handle, 0, 0, 0);
                sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_MODE, fseek_fd, NULL, NULL, NULL, fseek_mode, 0, 0, 0);
                sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_BACKEND, fseek_fd, NULL, NULL, NULL, 0, 0,
                                   (uint8_t)file->backend, 0);
                {
                    char off_in_str[24];
                    snprintf(off_in_str, sizeof(off_in_str), "in=%ld", (long)(int32_t)fseek_offset);
                    sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_OFFSET_IN, fseek_fd, off_in_str, NULL, NULL, 0, 0, 0, 0);
                }

                bool fseek_ok = false;
                uint32_t fseek_new_offset = file->offset;
                uint8_t fseek_tnfs_rc = 0xFFu;
                switch (fseek_mode)
                {
                case 0: // SEEK_SET
                {
                    uint32_t target = fseek_offset;
                    fseek_ok = sidetnfs_tnfs_file_seek(fseek_fd, file->tnfs_handle, false, (int32_t)target,
                                                        &fseek_new_offset, &fseek_tnfs_rc);
                    break;
                }
                case 1: // SEEK_CUR
                {
                    uint32_t target = file->offset + fseek_offset;
                    fseek_ok = sidetnfs_tnfs_file_seek(fseek_fd, file->tnfs_handle, false, (int32_t)target,
                                                        &fseek_new_offset, &fseek_tnfs_rc);
                    break;
                }
                case 2: // SEEK_END
                {
                    fseek_ok = sidetnfs_tnfs_file_seek(fseek_fd, file->tnfs_handle, true, (int32_t)fseek_offset,
                                                        &fseek_new_offset, &fseek_tnfs_rc);
                    break;
                }
                default:
                    DPRINTF("ERROR: Invalid Fseek mode for TNFS-backed fd %x: %x\n", fseek_fd, fseek_mode);
                    fseek_ok = false;
                    break;
                }

                // Fase 7F-debugfix: always recorded, independent of the
                // eventlog's fixed-size budget (see
                // sidetnfs_note_tnfs_fseek()).
                sidetnfs_note_tnfs_fseek(fseek_mode, fseek_fd, fseek_tnfs_rc, fseek_ok);

                if (!fseek_ok)
                {
                    DPRINTF("ERROR: TNFS seek failed for fd %x\n", fseek_fd);
                    sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_RETURN, fseek_fd, NULL, NULL, NULL, 0, 0,
                                       (uint8_t)GEMDOS_EINTRN, 0);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FSEEK_STATUS, GEMDOS_EINTRN);
                }
                else
                {
                    file->offset = fseek_new_offset;
                    char off_out_str[24];
                    snprintf(off_out_str, sizeof(off_out_str), "out=%lu", (unsigned long)fseek_new_offset);
                    sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_OFFSET_OUT, fseek_fd, off_out_str, NULL, NULL, 0, 0, 0, 0);
                    sidetnfs_diag_log(SIDETNFS_DIAG_FSEEK_RETURN, fseek_fd, NULL, NULL, NULL, 0, 0, 0, 0);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FSEEK_STATUS, fseek_new_offset);
                }
            }
#endif
#if SIDETNFS_CONFIG_DRIVE_ONLY
            else if (file->backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH)
            {
                // Fase 10B: pure RAM offset arithmetic -- no I/O, no
                // interrupts disabled. SEEK_END uses the known flash-array
                // length instead of f_size() (there is no FIL object for
                // this backend). Clamped to [0, config_flash_size] so a
                // later Fread's `remaining` computation can never
                // underflow -- checked explicitly against the real
                // 132992-byte SIDETNFS.PRG (see report), since Pexec's own
                // loader may Fseek non-linearly, not just read forward.
                int64_t new_offset;
                switch (fseek_mode)
                {
                case 0: // SEEK_SET
                    new_offset = (int64_t)fseek_offset;
                    break;
                case 1: // SEEK_CUR
                    new_offset = (int64_t)file->offset + (int32_t)fseek_offset;
                    break;
                case 2: // SEEK_END
                    new_offset = (int64_t)file->config_flash_size + (int32_t)fseek_offset;
                    break;
                default:
                    new_offset = (int64_t)file->offset;
                    break;
                }
                if (new_offset < 0)
                {
                    new_offset = 0;
                }
                if (new_offset > (int64_t)file->config_flash_size)
                {
                    new_offset = (int64_t)file->config_flash_size;
                }
                file->offset = (uint32_t)new_offset;
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FSEEK_STATUS, file->offset);
            }
#endif
            else
            {
                switch (fseek_mode)
                {
                case 0: // SEEK_SET 0 offset specifies the positive number of bytes from the beginning of the file
                    file->offset = fseek_offset;
                    break;
                case 1: // SEEK_CUR 1 offset specifies offset specifies the negative or positive number of bytes from the current file position
                    file->offset += fseek_offset;
                    if (file->offset < 0)
                    {
                        file->offset = 0;
                    }
                    break;
                case 2: // SEEK_END 2 offset specifies the negative number of bytes from the end of the file
                    // Get file size
                    file->offset = f_size(&(file->fobject)) + fseek_offset;
                    if (file->offset < 0)
                    {
                        file->offset = 0;
                    }
                    break;
                }
                // We don't really need to do the lseek here, because it will be performed in the read operation
                if (fr != FR_OK)
                {
                    DPRINTF("ERROR: Could not seek file (%d)\r\n", fr);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FSEEK_STATUS, GEMDOS_EINTRN);
                }
                else
                {
                    DPRINTF("File seeked\n");
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FSEEK_STATUS, file->offset);
                }
            }
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_FATTRIB_CALL:
        {
            uint16_t fattrib_flag = payloadPtr[0]; // d3 register
            payloadPtr += 2;                       // Skip two words
            // Obtain the new attributes, if FATTRIB_SET is set
            uint16_t fattrib_new = payloadPtr[0]; // d4 register
            payloadPtr += 4;                      // Skip four words
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
            // Fase 10B: root-only, read-only drive. Inquire returns the
            // fixed READONLY|ARCH attribute for the two known files (see
            // sidetnfs_config_drive_backend.c's SIDETNFS_CONFIG_DRIVE_ATTR);
            // Fattrib SET is always rejected -- there is nothing to change,
            // and no flash write ever happens from this backend.
            char config_fattrib_raw_path[MAX_FOLDER_LENGTH] = {0};
            char config_fattrib_path[MAX_FOLDER_LENGTH] = {0};
            get_tnfs_relative_pathname(config_fattrib_path, config_fattrib_raw_path, NULL);
            char config_fattrib_name83[14];
            const uint8_t *config_fattrib_data;
            uint32_t config_fattrib_size;
            bool config_fattrib_found = config_flash_root_name(config_fattrib_path, config_fattrib_name83,
                                                                  sizeof(config_fattrib_name83)) &&
                                         sidetnfs_config_drive_lookup(config_fattrib_name83, &config_fattrib_data,
                                                                       &config_fattrib_size);
            if (fattrib_flag == FATTRIB_INQUIRE)
            {
                if (config_fattrib_found)
                {
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS,
                                             (uint32_t)SIDETNFS_CONFIG_DRIVE_ATTR);
                }
                else
                {
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EFILNF);
                }
            }
            else
            {
                // FATTRIB_SET -- read-only drive, never permitted.
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS,
                                         config_fattrib_found ? GEMDOS_EACCDN : GEMDOS_EFILNF);
            }
#else
            // Fase 7L/7Lb: real TNFS Fattrib inquire via STAT (see
            // sidetnfs_tnfs_get_attributes() in sidetnfs_probe.c for the
            // wire-level detail). Fattrib SET is unconditionally reported
            // as unsupported -- confirmed against the actual server
            // source (tnfsd 24.0522.1): it registers command 0x27
            // (TNFS_CHMODFILE) in its dispatch table, but the handler
            // function body is empty (no payload parse, no chmod(), no
            // response), so sending it would only ever time out. TNFS
            // Fattrib set is therefore reported as unsupported.
            // STAT-based inquiry remains supported. Never touches the
            // physical SD card.
            char tnfs_fattrib_raw_path[MAX_FOLDER_LENGTH] = {0};
            char tnfs_fattrib_path[MAX_FOLDER_LENGTH] = {0};
            get_tnfs_relative_pathname(tnfs_fattrib_path, tnfs_fattrib_raw_path, NULL);
            DPRINTF("TNFS Fattrib flag: %x, new attributes: %x, path: %s\n", fattrib_flag, fattrib_new,
                     tnfs_fattrib_path);

            if (fattrib_flag == FATTRIB_INQUIRE)
            {
                uint8_t tnfs_fattrib_st_attribs = 0;
                uint8_t tnfs_fattrib_rc = 0xFFu;
                SidetnfsAttrResult tnfs_fattrib_result =
                    sidetnfs_tnfs_get_attributes(tnfs_fattrib_path, &tnfs_fattrib_st_attribs, &tnfs_fattrib_rc);
                bool tnfs_fattrib_ok = tnfs_fattrib_result == SIDETNFS_ATTR_OK;
                sidetnfs_note_tnfs_fattrib(fattrib_flag, 0, tnfs_fattrib_st_attribs, tnfs_fattrib_rc, tnfs_fattrib_ok,
                                            false, tnfs_fattrib_path);
                switch (tnfs_fattrib_result)
                {
                case SIDETNFS_ATTR_OK:
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS,
                                             (uint32_t)tnfs_fattrib_st_attribs);
                    break;
                case SIDETNFS_ATTR_NOT_FOUND:
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EFILNF);
                    break;
                case SIDETNFS_ATTR_PATH_NOT_FOUND:
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EPTHNF);
                    break;
                case SIDETNFS_ATTR_ACCESS_DENIED:
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EACCDN);
                    break;
                case SIDETNFS_ATTR_ERROR:
                default:
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EINTRN);
                    break;
                }
            }
            else
            {
                // Fase 7Lb: mask is passed through for diagnostic/logging
                // purposes only -- sidetnfs_tnfs_set_attributes() always
                // reports SIDETNFS_ATTR_ACCESS_DENIED/unsupported now (see
                // its own header comment), never actually applies
                // anything, and never contacts the server.
                uint8_t tnfs_fattrib_result_attribs = 0;
                uint8_t tnfs_fattrib_rc = 0xFFu;
                bool tnfs_fattrib_unsupported = false;
                SidetnfsAttrResult tnfs_fattrib_result = sidetnfs_tnfs_set_attributes(
                    tnfs_fattrib_path, (uint8_t)fattrib_new,
                    (uint8_t)(FS_ST_READONLY | FS_ST_HIDDEN | FS_ST_SYSTEM), &tnfs_fattrib_result_attribs,
                    &tnfs_fattrib_rc, &tnfs_fattrib_unsupported);
                bool tnfs_fattrib_ok = tnfs_fattrib_result == SIDETNFS_ATTR_OK;
                sidetnfs_note_tnfs_fattrib(fattrib_flag, (uint8_t)fattrib_new, tnfs_fattrib_result_attribs,
                                            tnfs_fattrib_rc, tnfs_fattrib_ok, tnfs_fattrib_unsupported,
                                            tnfs_fattrib_path);
                switch (tnfs_fattrib_result)
                {
                case SIDETNFS_ATTR_OK:
                    // Fase 7Lb: unreachable with the current server (see
                    // sidetnfs_tnfs_set_attributes(), which always returns
                    // SIDETNFS_ATTR_ACCESS_DENIED) -- kept only so this
                    // switch stays exhaustive/correct if a future TNFS
                    // server ever implements CHMOD for real. Would return
                    // the NEW (just-applied) attributes, the standard
                    // documented GEMDOS Fattrib behavior.
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS,
                                             (uint32_t)tnfs_fattrib_result_attribs);
                    break;
                case SIDETNFS_ATTR_NOT_FOUND:
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EFILNF);
                    break;
                case SIDETNFS_ATTR_PATH_NOT_FOUND:
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EPTHNF);
                    break;
                case SIDETNFS_ATTR_ACCESS_DENIED:
                    // Fase 7Lb: the actual path taken today -- TNFS
                    // Fattrib set is unconditionally unsupported on this
                    // server (see sidetnfs_tnfs_set_attributes()).
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EACCDN);
                    break;
                case SIDETNFS_ATTR_ERROR:
                default:
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EINTRN);
                    break;
                }
            }
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
            // Obtain the fname string and keep it in memory
            // concatenated path and filename
            char tmp_filepath[MAX_FOLDER_LENGTH] = {0};
            get_local_full_pathname(tmp_filepath);
            DPRINTF("Fattrib flag: %x, new attributes: %x\n", fattrib_flag, fattrib_new);
            DPRINTF("Getting attributes of file: %s\n", tmp_filepath);

            // Get the attributes of the file
            ScFsStat stat_info;
            bool stat_ok = scfs_stat(tmp_filepath, &stat_info);
            if (!stat_ok)
            {
                DPRINTF("ERROR: Could not get file attributes\r\n");
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EFILNF);
            }
            else
            {
                uint32_t fattrib_st = attribs_fat2st(stat_info.attr);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, fattrib_st);
                char fattrib_st_str[7] = "";
                get_attribs_st_str(fattrib_st_str, fattrib_st);
                if (fattrib_flag == FATTRIB_INQUIRE)
                {
                    DPRINTF("File attributes: %s\n", fattrib_st_str);
                }
                else
                {
                    // WE will assume here FATTRIB_SET
                    // Set the attributes of the file
                    char fattrib_st_str[7] = "";
                    get_attribs_st_str(fattrib_st_str, fattrib_new);
                    DPRINTF("New file attributes: %s\n", fattrib_st_str);
                    BYTE fattrib_fatfs_new = (BYTE)attribs_st2fat(fattrib_new);
                    fr = f_chmod(tmp_filepath, fattrib_fatfs_new, AM_RDO | AM_HID | AM_SYS);
                    if (fr != FR_OK)
                    {
                        DPRINTF("ERROR: Could not set file attributes (%d)\r\n", fr);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FATTRIB_STATUS, GEMDOS_EACCDN);
                    }
                }
            }
#endif
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_FRENAME_CALL:
        {
            payloadPtr += 6; // Skip six words
            // Obtain the src name from the payload
            char *origin = (char *)payloadPtr;
            char frename_fname_src[MAX_FOLDER_LENGTH] = {0};
            char frename_fname_dst[MAX_FOLDER_LENGTH] = {0};
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(origin, frename_fname_src, MAX_FOLDER_LENGTH);
            COPY_AND_CHANGE_ENDIANESS_BLOCK16(origin + MAX_FOLDER_LENGTH, frename_fname_dst, MAX_FOLDER_LENGTH);

            char drive_src[3] = {0};
            char folders_src[MAX_FOLDER_LENGTH] = {0};
            char filePattern_src[MAX_FOLDER_LENGTH] = {0};
            char drive_dst[3] = {0};
            char folders_dst[MAX_FOLDER_LENGTH] = {0};
            char filePattern_dst[MAX_FOLDER_LENGTH] = {0};
            split_fullpath(frename_fname_src, drive_src, folders_src, filePattern_src);
            DPRINTF("Drive: %s, Folders: %s, FilePattern: %s\n", drive_src, folders_src, filePattern_src);
            split_fullpath(frename_fname_dst, drive_dst, folders_dst, filePattern_dst);
            DPRINTF("Drive: %s, Folders: %s, FilePattern: %s\n", drive_dst, folders_dst, filePattern_dst);

            if (strcasecmp(drive_src, drive_dst) != 0)
            {
                DPRINTF("ERROR: Different drives\n");
                *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS)) = SWAP_LONGWORD(GEMDOS_EPTHNF);
            }
#if SIDETNFS_USE_TNFS_LISTING
#if SIDETNFS_CONFIG_DRIVE_ONLY
            else
            {
                // Fase 10B: read-only drive -- Frename is always denied,
                // never even parses the destination path (nothing to rename).
                *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS)) =
                    SWAP_LONGWORD(GEMDOS_EACCDN);
            }
#else
            else
            {
                // Fase 7H: real TNFS rename. payloadPtr is still at the src
                // raw string here (unchanged by the COPY_AND_CHANGE_ENDIANESS_BLOCK16
                // calls above, which read via the local `origin` copy, not
                // payloadPtr itself) -- same precondition the SD route
                // below relies on for its own get_local_full_pathname()
                // calls.
                char tnfs_rename_raw_src[MAX_FOLDER_LENGTH] = {0};
                char tnfs_rename_raw_dst[MAX_FOLDER_LENGTH] = {0};
                char tnfs_rename_src[MAX_FOLDER_LENGTH] = {0};
                char tnfs_rename_dst[MAX_FOLDER_LENGTH] = {0};
                get_tnfs_relative_pathname(tnfs_rename_src, tnfs_rename_raw_src, NULL);
                payloadPtr += MAX_FOLDER_LENGTH / 2; // MAX_FOLDER_LENGTH * 2 bytes per uint16_t
                get_tnfs_relative_pathname(tnfs_rename_dst, tnfs_rename_raw_dst, NULL);

                sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_ENTER, 0, NULL, NULL, NULL, 0, 0, 0, 0);
                sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_RAW_SRC, 0, tnfs_rename_raw_src, NULL, NULL, 0, 0, 0, 0);
                sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_RAW_DST, 0, tnfs_rename_raw_dst, NULL, NULL, 0, 0, 0, 0);
                sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_TNFS_SRC, 0, tnfs_rename_src, NULL, NULL, 0, 0, 0, 0);
                sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_TNFS_DST, 0, tnfs_rename_dst, NULL, NULL, 0, 0, 0, 0);

                uint8_t tnfs_rename_rc = 0xFFu;
                SidetnfsFileRenameResult tnfs_rename_result =
                    sidetnfs_tnfs_file_rename(tnfs_rename_src, tnfs_rename_dst, &tnfs_rename_rc);
                sidetnfs_note_tnfs_frename(tnfs_rename_src, tnfs_rename_dst, tnfs_rename_rc,
                                            tnfs_rename_result == SIDETNFS_FILE_RENAME_OK);

                uint32_t tnfs_rename_status;
                switch (tnfs_rename_result)
                {
                case SIDETNFS_FILE_RENAME_OK:
                {
                    tnfs_rename_status = GEMDOS_EOK;
                    // Fase 7H: targeted registration update, not a broad
                    // cache reset -- a TNFS-backed handle currently open
                    // under the OLD path stays perfectly valid after a
                    // server-side rename (the TNFS handle refers to the
                    // open file object, not the path), so we only correct
                    // the local fpath bookkeeping to the new name instead
                    // of closing it. Fsfirst/Fsnext never cache directory
                    // entries (see Fase 5Y/6B report), so no
                    // directory-search state needs touching either.
                    FileDescriptors *tnfs_rename_open_file = get_file_by_fpath(fdescriptors, tnfs_rename_src);
                    if (tnfs_rename_open_file != NULL && tnfs_rename_open_file->backend == GEMDRIVE_FILE_BACKEND_TNFS)
                    {
                        strncpy(tnfs_rename_open_file->fpath, tnfs_rename_dst,
                                sizeof(tnfs_rename_open_file->fpath) - 1);
                        tnfs_rename_open_file->fpath[sizeof(tnfs_rename_open_file->fpath) - 1] = '\0';
                        sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_HANDLE_UPDATE, (uint32_t)tnfs_rename_open_file->fd,
                                           tnfs_rename_dst, NULL, NULL, 0, 0, 0, 0);
                    }
                    break;
                }
                case SIDETNFS_FILE_RENAME_NOT_FOUND:
                    tnfs_rename_status = GEMDOS_EFILNF;
                    break;
                case SIDETNFS_FILE_RENAME_ACCESS_DENIED:
                    tnfs_rename_status = GEMDOS_EACCDN;
                    break;
                case SIDETNFS_FILE_RENAME_ERROR:
                default:
                    tnfs_rename_status = GEMDOS_EINTRN;
                    break;
                }
                sidetnfs_diag_log(SIDETNFS_DIAG_FRENAME_RETURN, 0, tnfs_rename_dst, NULL, NULL, 0, 0,
                                   (uint8_t)tnfs_rename_status, 0);
                *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS)) =
                    SWAP_LONGWORD(tnfs_rename_status);
            }
#endif // SIDETNFS_CONFIG_DRIVE_ONLY
#else
            else
            {
                DPRINTF("Renaming file: %s to %s\n", frename_fname_src, frename_fname_dst);
                get_local_full_pathname(frename_fname_src);
                payloadPtr += MAX_FOLDER_LENGTH / 2; // MAX_FOLDER_LENGTH * 2 bytes per uint16_t
                get_local_full_pathname(frename_fname_dst);
                DPRINTF("Renaming file: %s to %s\n", frename_fname_src, frename_fname_dst);
                // Rename the file
                fr = f_rename(frename_fname_src, frename_fname_dst);
                if (fr != FR_OK)
                {
                    DPRINTF("ERROR: Could not rename file (%d)\r\n", fr);
                    if (fr == FR_DENIED)
                    {
                        DPRINTF("ERROR: Not enough premissions to rename file\n");
                        *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS)) = SWAP_LONGWORD(GEMDOS_EACCDN);
                    }
                    else if (fr == FR_NO_PATH)
                    {
                        DPRINTF("ERROR: Folder does not exist\n");
                        *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS)) = SWAP_LONGWORD(GEMDOS_EPTHNF);
                    }
                    else if (fr == FR_NO_FILE)
                    {
                        DPRINTF("ERROR: File does not exist\n");
                        *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS)) = SWAP_LONGWORD(GEMDOS_EFILNF);
                    }
                    else
                    {
                        DPRINTF("ERROR: Internal error\n");
                        *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS)) = SWAP_LONGWORD(GEMDOS_EINTRN);
                    }
                }
                else
                {
                    DPRINTF("File renamed\n");
                    *((volatile uint32_t *)(memory_shared_address + GEMDRVEMUL_FRENAME_STATUS)) = GEMDOS_EOK;
                }
            }
#endif

            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_FDATETIME_CALL:
        {
            uint16_t fdatetime_flag = payloadPtr[0]; // d3.w register
            payloadPtr += 2;                         // Skip two words
            // Obtain the file descriptor to change the date and time
            uint16_t fdatetime_fd = payloadPtr[0]; // d4 register
            payloadPtr += 2;                       // Skip two words
            // Obtain the date and time to set
            uint16_t date_dos = payloadPtr[0]; // d5 low register
            uint16_t time_dos = payloadPtr[1]; // d5 high register
            DPRINTF("Fdatetime flag: %x, fd: %x, time: %x, date: %x\n", fdatetime_flag, fdatetime_fd, time_dos, date_dos);

            FileDescriptors *fd = get_file_by_fdesc(fdescriptors, fdatetime_fd);
            if (fd == NULL)
            {
                DPRINTF("ERROR: File descriptor not found\n");
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EIHNDL);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
            }
#if SIDETNFS_USE_TNFS_LISTING
            else if (fd->backend == GEMDRIVE_FILE_BACKEND_TNFS)
            {
                // Fase 7M: real TNFS Fdatime inquire via STAT (see
                // sidetnfs_tnfs_get_datetime() in sidetnfs_probe.c).
                // fd->fpath already holds the TNFS-relative path this
                // handle was opened with (set at Fopen/Fcreate time) -- no
                // separate path lookup or new handle-table field needed.
                // Set is unconditionally unsupported -- confirmed against
                // the actual server source (tnfsd 24.0522.1) that its
                // protocol header defines no UTIME/SETTIME command at all.
                if (fdatetime_flag == FDATETIME_INQUIRE)
                {
                    DPRINTF("TNFS inquire file date and time: %s fd: %d\n", fd->fpath, fdatetime_fd);
                    uint16_t tnfs_fdatime_gemdos_date = 0;
                    uint16_t tnfs_fdatime_gemdos_time = 0;
                    uint32_t tnfs_fdatime_unix_mtime = 0;
                    uint8_t tnfs_fdatime_rc = 0xFFu;
                    SidetnfsAttrResult tnfs_fdatime_result =
                        sidetnfs_tnfs_get_datetime(fd->fpath, &tnfs_fdatime_gemdos_date, &tnfs_fdatime_gemdos_time,
                                                    &tnfs_fdatime_unix_mtime, &tnfs_fdatime_rc);
                    bool tnfs_fdatime_ok = tnfs_fdatime_result == SIDETNFS_ATTR_OK;
                    sidetnfs_note_tnfs_fdatime(fdatetime_flag, (uint8_t)fdatetime_fd, fd->fpath, tnfs_fdatime_rc,
                                                tnfs_fdatime_ok, false, tnfs_fdatime_unix_mtime,
                                                tnfs_fdatime_gemdos_date, tnfs_fdatime_gemdos_time);
                    switch (tnfs_fdatime_result)
                    {
                    case SIDETNFS_ATTR_OK:
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EOK);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE,
                                                 tnfs_fdatime_gemdos_date);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME,
                                                 tnfs_fdatime_gemdos_time);
                        break;
                    case SIDETNFS_ATTR_NOT_FOUND:
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EFILNF);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                        break;
                    case SIDETNFS_ATTR_PATH_NOT_FOUND:
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EPTHNF);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                        break;
                    case SIDETNFS_ATTR_ACCESS_DENIED:
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EACCDN);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                        break;
                    case SIDETNFS_ATTR_ERROR:
                    default:
                        // Fase 7M: covers both a genuine transport/server
                        // error and a too-short STAT response (see
                        // sidetnfs_tnfs_get_datetime()/tnfs_stat_raw()) --
                        // never a partial date/time written either way.
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EINTRN);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                        break;
                    }
                }
                else
                {
                    DPRINTF("TNFS modify file date and time (unsupported): %s fd: %d\n", fd->fpath, fdatetime_fd);
                    sidetnfs_tnfs_set_datetime_unsupported(fd->fpath, date_dos, time_dos);
                    sidetnfs_note_tnfs_fdatime(fdatetime_flag, (uint8_t)fdatetime_fd, fd->fpath, 0xFFu, false, true,
                                                0, date_dos, time_dos);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EACCDN);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                }
            }
#if SIDETNFS_CONFIG_DRIVE_ONLY
            else if (fd->backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH)
            {
                // Fase 10B: fixed placeholder date/time for both embedded
                // files (see SIDETNFS_CONFIG_DRIVE_DATE/TIME) -- inquire
                // always succeeds, set is always rejected (read-only drive).
                if (fdatetime_flag == FDATETIME_INQUIRE)
                {
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EOK);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE,
                                             SIDETNFS_CONFIG_DRIVE_DATE);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME,
                                             SIDETNFS_CONFIG_DRIVE_TIME);
                }
                else
                {
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EACCDN);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                }
            }
#endif
#endif
            else
            {
                if (fdatetime_flag == FDATETIME_INQUIRE)
                {
                    DPRINTF("Inquire file date and time: %s fd: %d\n", fd->fpath, fdatetime_fd);
                    ScFsStat stat_info;
                    bool stat_ok = scfs_stat(fd->fpath, &stat_info);
                    if (stat_ok)
                    {
                        // File information is now in stat_info
#if defined(_DEBUG) && (_DEBUG != 0)
                        // Save some memory and cycles if not in debug mode
                        // Convert the date and time
                        unsigned int year = (stat_info.date >> 9);
                        unsigned int month = (stat_info.date >> 5) & 0x0F;
                        unsigned int day = stat_info.date & 0x1F;

                        unsigned int hour = stat_info.time >> 11;
                        unsigned int minute = (stat_info.time >> 5) & 0x3F;
                        unsigned int second = (stat_info.time & 0x1F);

                        DPRINTF("Get file date and time: %02d:%02d:%02d %02d/%02d/%02d\n", hour, minute, second * 2, day, month, year + 1980);
#endif
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EOK);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, stat_info.date);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, stat_info.time);
                    }
                    else
                    {
                        DPRINTF("ERROR: Could not get file date and time from file %s\r\n", fd->fpath);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EFILNF);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                    }
                }
                else
                {
                    DPRINTF("Modify file date and time: %s fd: %d\n", fd->fpath, fdatetime_fd);
#if defined(_DEBUG) && (_DEBUG != 0)
                    // Save some memory and cycles if not in debug mode
                    // Convert the date and time
                    unsigned int year = (date_dos >> 9);
                    unsigned int month = (date_dos >> 5) & 0x0F;
                    unsigned int day = date_dos & 0x1F;

                    unsigned int hour = time_dos >> 11;
                    unsigned int minute = (time_dos >> 5) & 0x3F;
                    unsigned int second = (time_dos & 0x1F);

                    DPRINTF("Show in hex the values: %02x:%02x:%02x %02x/%02x/%02x\n", hour, minute, second, day, month, year);
                    DPRINTF("File date and time: %02d:%02d:%02d %02d/%02d/%02d\n", hour, minute, second * 2, day, month, year + 1980);
#endif
                    FILINFO fno;
                    fno.fdate = date_dos;
                    fno.ftime = time_dos;
                    fr = f_utime(fd->fpath, &fno);
                    if (fr == FR_OK)
                    {
                        // File exists and date and time set
                        // So now we can return the status
                        DPRINTF("Set the file date and time: %02d:%02d:%02d %02d/%02d/%02d\n", hour, minute, second * 2, day, month, year + 1980);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EOK);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                    }
                    else
                    {
                        DPRINTF("ERROR: Could not set file date and time to file %s (%d)\r\n", fd->fpath, fr);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_STATUS, GEMDOS_EFILNF);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_DATE, 0);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_FDATETIME_TIME, 0);
                    }
                }
            }
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }

        case GEMDRVEMUL_READ_BUFF_CALL:
        {
            uint16_t readbuff_fd = payloadPtr[0];                                                      // d3 register
            payloadPtr += 2;                                                                           // Skip two words
            uint32_t readbuff_bytes_to_read = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];         // d4 register constains the number of bytes to read
            payloadPtr += 2;                                                                           // Skip two words
            uint32_t readbuff_pending_bytes_to_read = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d5 register constains the number of bytes to read
            DPRINTF("Read buffering file with fd: x%x, bytes_to_read: x%08x, pending_bytes_to_read: x%08x\n", readbuff_fd, readbuff_bytes_to_read, readbuff_pending_bytes_to_read);
            // Show open files
#if defined(_DEBUG) && (_DEBUG != 0)
            print_file_descriptors(fdescriptors);
#endif
            gemdrive_backend_fread(readbuff_fd, readbuff_pending_bytes_to_read, memory_shared_address);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_WRITE_BUFF_CALL:
        {
            uint16_t writebuff_fd = payloadPtr[0];                                                       // d3 register
            payloadPtr += 2;                                                                             // Skip two words
            uint32_t writebuff_bytes_to_write = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];         // d4 register constains the number of bytes to write
            payloadPtr += 2;                                                                             // Skip two words
            uint32_t writebuff_pending_bytes_to_write = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d5 register constains the number of bytes to write
            payloadPtr += 2;
            DPRINTF("Write buffering file with fd: x%x, bytes_to_write: x%08x, pending_bytes_to_write: x%08x\n", writebuff_fd, writebuff_bytes_to_write, writebuff_pending_bytes_to_write);
            // Obtain the file descriptor
            FileDescriptors *file = get_file_by_fdesc(fdescriptors, writebuff_fd);
            if (file == NULL)
            {
                DPRINTF("ERROR: File descriptor not found\n");
#if SIDETNFS_USE_TNFS_LISTING
                sidetnfs_diag_log(SIDETNFS_DIAG_FWRITE_BAD_HANDLE, writebuff_fd, NULL, NULL, NULL, 0, 0, 0xFFu, 0);
                sidetnfs_note_tnfs_fwrite(0, (uint16_t)writebuff_pending_bytes_to_write, 0, 0xFFu, false, false);
#endif
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, GEMDOS_EIHNDL);
            }
#if SIDETNFS_USE_TNFS_LISTING
            else if (file->backend == GEMDRIVE_FILE_BACKEND_TNFS)
            {
                // Fase 7K: real TNFS write. Checksum/endianness handling is
                // unchanged from the SD/FatFS route below (68k-side
                // handshake is backend-agnostic) -- only the actual
                // byte-write and the offset-repositioning that precedes it
                // are TNFS instead of FatFS.
                if (!file->tnfs_writable)
                {
                    DPRINTF("ERROR: TNFS handle fd %x is read-only\n", writebuff_fd);
                    sidetnfs_diag_log(SIDETNFS_DIAG_FWRITE_READONLY, writebuff_fd, NULL, NULL, NULL,
                                       file->tnfs_handle, 0, 0, 0);
                    sidetnfs_note_tnfs_fwrite(file->tnfs_handle, (uint16_t)writebuff_pending_bytes_to_write, 0,
                                               0xFFu, false, false);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, GEMDOS_EACCDN);
                }
                else
                {
                    uint32_t writebuff_offset = file->offset;
                    uint16_t buff_size = writebuff_pending_bytes_to_write > DEFAULT_FWRITE_BUFFER_SIZE
                                              ? DEFAULT_FWRITE_BUFFER_SIZE
                                              : (uint16_t)writebuff_pending_bytes_to_write;
                    uint16_t *target = payloadPtr;
                    // Checksum computed exactly like the SD/FatFS route
                    // below -- over the whole fixed DEFAULT_FWRITE_BUFFER_SIZE
                    // region regardless of buff_size, since the 68k side
                    // verifies it against the source buffer it sent, not
                    // against how many bytes actually ended up written.
                    uint16_t chk = 0;
                    UINT words_to_write = (DEFAULT_FWRITE_BUFFER_SIZE) / 2;
                    UINT pending_bytes = (DEFAULT_FWRITE_BUFFER_SIZE) % 2;
                    uint16_t *target16 = (uint16_t *)target;
                    for (int i = 0; i < words_to_write; i++)
                    {
                        chk += target16[i];
                    }
                    if (pending_bytes > 0)
                    {
                        uint16_t pending_long_word = target16[words_to_write];
                        chk += pending_long_word & (0x00FF << (pending_bytes * 8));
                    }
                    DPRINTF("Checksum: x%x\n", chk);

                    if (buff_size == 0)
                    {
                        // Nothing requested this call -- no TNFS traffic
                        // needed (e.g. an Fwrite with count 0).
                        sidetnfs_note_tnfs_fwrite(file->tnfs_handle, 0, 0, 0, true, false);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_CHK, (uint32_t)chk);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, 0);
                    }
                    else
                    {
                        // Change the endianness of the bytes to write
                        CHANGE_ENDIANESS_BLOCK16(target, buff_size + (buff_size % 2));

                        // Reposition the TNFS file cursor to file->offset
                        // first -- mirrors f_lseek()'s always-seek-before-write
                        // behavior below exactly, so a retry (68k re-sends
                        // the same chunk without having confirmed/advanced
                        // the offset via GEMDRVEMUL_WRITE_BUFF_CHECK)
                        // correctly overwrites the same spot instead of
                        // silently continuing past it.
                        uint32_t writebuff_new_offset = writebuff_offset;
                        uint8_t writebuff_seek_rc = 0xFFu;
                        bool writebuff_seek_ok =
                            sidetnfs_tnfs_file_seek(writebuff_fd, file->tnfs_handle, false,
                                                     (int32_t)writebuff_offset, &writebuff_new_offset,
                                                     &writebuff_seek_rc);
                        if (!writebuff_seek_ok)
                        {
                            DPRINTF("ERROR: Could not seek TNFS file before write (fd %x)\n", writebuff_fd);
                            sidetnfs_diag_log(SIDETNFS_DIAG_FWRITE_TRANSPORT_ERROR, writebuff_fd, NULL, NULL, NULL,
                                               file->tnfs_handle, 0, writebuff_seek_rc, 0);
                            sidetnfs_note_tnfs_fwrite(file->tnfs_handle, buff_size, 0, writebuff_seek_rc, false,
                                                       false);
                            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, GEMDOS_EINTRN);
                        }
                        else
                        {
                            uint16_t bytes_write = 0;
                            uint8_t writebuff_rc = 0xFFu;
                            bool writebuff_ok = sidetnfs_tnfs_file_write(writebuff_fd, file->tnfs_handle,
                                                                          (const uint8_t *)target, buff_size,
                                                                          &bytes_write, &writebuff_rc);
                            bool writebuff_partial = writebuff_ok && bytes_write < buff_size;
                            sidetnfs_note_tnfs_fwrite(file->tnfs_handle, buff_size, bytes_write, writebuff_rc,
                                                       writebuff_ok, writebuff_partial);
                            if (writebuff_partial)
                            {
                                sidetnfs_diag_log(SIDETNFS_DIAG_FWRITE_PARTIAL, writebuff_fd, NULL, NULL, NULL,
                                                   file->tnfs_handle, bytes_write, writebuff_rc, 0);
                            }
                            if (!writebuff_ok)
                            {
                                DPRINTF("ERROR: Could not write TNFS file (fd %x)\n", writebuff_fd);
                                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, GEMDOS_EINTRN);
                            }
                            else
                            {
                                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_CHK, (uint32_t)chk);
                                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES,
                                                         (uint32_t)bytes_write);
                            }
                        }
                    }
                }
            }
#if SIDETNFS_CONFIG_DRIVE_ONLY
            else if (file->backend == GEMDRIVE_FILE_BACKEND_CONFIG_FLASH)
            {
                // Fase 10B: read-only drive -- never touches file->fobject
                // (never opened via f_open() for this backend), unlike the
                // SD/FatFS fallback below which would call f_lseek() on it.
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, GEMDOS_EACCDN);
            }
#endif
#endif
            else
            {
                uint32_t writebuff_offset = file->offset;
                UINT bytes_write = 0;
                // Reposition the file pointer with FatFs
                fr = f_lseek(&file->fobject, writebuff_offset);
                if (fr != FR_OK)
                {
                    DPRINTF("ERROR: Could not change write offset of the file (%d)\r\n", fr);
                    WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, GEMDOS_EINTRN);
                }
                else
                {
                    // Only write DEFAULT_FWRITE_BUFFER_SIZE bytes at a time
                    uint16_t buff_size = writebuff_pending_bytes_to_write > DEFAULT_FWRITE_BUFFER_SIZE ? DEFAULT_FWRITE_BUFFER_SIZE : writebuff_pending_bytes_to_write;
                    // Transform buffer's words from little endian to big endian inline
                    uint16_t *target = payloadPtr;
                    // Calculate the checksum of the buffer
                    // Use a 16 bit checksum to minimize the number of loops
                    uint16_t chk = 0;
                    UINT words_to_write = (DEFAULT_FWRITE_BUFFER_SIZE) / 2;
                    UINT pending_bytes = (DEFAULT_FWRITE_BUFFER_SIZE) % 2;
                    uint16_t *target16 = (uint16_t *)target;
                    for (int i = 0; i < words_to_write; i++)
                    {
                        // Swap the order of the bytes in target16
                        chk += target16[i];
                    }
                    DPRINTF("Checksum: x%x\n", chk);
                    if (pending_bytes > 0)
                    {
                        uint16_t pending_long_word = target16[words_to_write];
                        chk += pending_long_word & (0x00FF << (pending_bytes * 8));
                    }
                    // Change the endianness of the bytes read
                    CHANGE_ENDIANESS_BLOCK16(target, buff_size + (buff_size % 2));
                    // Write the bytes
                    DPRINTF("Write x%x bytes from the file at offset x%x\n", buff_size, writebuff_offset);
                    fr = f_write(&file->fobject, (void *)target, buff_size, &bytes_write);
                    if (fr != FR_OK)
                    {
                        DPRINTF("ERROR: Could not write file (%d)\r\n", fr);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, GEMDOS_EINTRN);
                    }
                    else
                    {
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_CHK, (uint32_t)chk);
                        WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_BYTES, bytes_write);
                    }
                }
            }
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_WRITE_BUFF_CHECK:
        {
            // Fase 7C (analysis, no change needed): this case only advances
            // the RAM-only FileDescriptors.offset bookkeeping field -- it
            // never calls f_write()/f_lseek() or otherwise touches the SD
            // card itself (the actual write already happened, or was
            // denied, in GEMDRVEMUL_WRITE_BUFF_CALL above). Safe to leave
            // unguarded at BACKEND_TNFS.
            uint16_t writebuff_fd = payloadPtr[0];                                              // d3 register
            payloadPtr += 2;                                                                    // Skip two words
            uint32_t writebuff_forward_bytes = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d4 register constains the number of bytes to forward the offset
            DPRINTF("Write buffering confirm fd: x%x, forward: x%08x\n", writebuff_fd, writebuff_forward_bytes);
            // Obtain the file descriptor
            FileDescriptors *file = get_file_by_fdesc(fdescriptors, writebuff_fd);
            if (file == NULL)
            {
                DPRINTF("ERROR: File descriptor not found\n");
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_CONFIRM_STATUS, GEMDOS_EIHNDL);
            }
            else
            {
                // Update the offset of the file
                file->offset += writebuff_forward_bytes;
                uint32_t current_offset = file->offset;
                DPRINTF("New offset: x%x after writing x%x bytes\n", current_offset, writebuff_forward_bytes);
                WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_WRITE_CONFIRM_STATUS, GEMDOS_EOK);
            }
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_PEXEC_CALL:
        {
            uint16_t pexec_mode = payloadPtr[0];                                         // d3 register
            payloadPtr += 2;                                                             // Skip 2 words
            uint32_t pexec_stack_addr = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0]; // d4 register
            payloadPtr += 2;                                                             // Skip 2 words
            uint32_t pexec_fname = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];      // d5 register
            payloadPtr += 2;                                                             // Skip 2 words
            uint32_t pexec_cmdline = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];    // d6 register
            payloadPtr += 2;                                                             // Skip 2 words
            uint32_t pexec_envstr = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];     // d7 register
            DPRINTF("Pexec mode: %x\n", pexec_mode);
            DPRINTF("Pexec stack addr: %x\n", pexec_stack_addr);
            DPRINTF("Pexec fname: %x\n", pexec_fname);
            DPRINTF("Pexec cmdline: %x\n", pexec_cmdline);
            DPRINTF("Pexec envstr: %x\n", pexec_envstr);
            *((volatile uint16_t *)(memory_shared_address + GEMDRVEMUL_PEXEC_MODE)) = pexec_mode;
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_PEXEC_STACK_ADDR, pexec_stack_addr);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_PEXEC_FNAME, pexec_fname);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_PEXEC_CMDLINE, pexec_cmdline);
            WRITE_AND_SWAP_LONGWORD(memory_shared_address, GEMDRVEMUL_PEXEC_ENVSTR, pexec_envstr);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SAVE_BASEPAGE:
        {
            payloadPtr += 6; // Skip eight words
            // Copy the from the shared memory the basepagea está to pexec_pd
            DPRINTF("Saving basepage\n");
            PD *origin = (PD *)(payloadPtr);
            // Reserve and copy the memory from origin to pexec_pd
            if (pexec_pd == NULL)
            {
                pexec_pd = (PD *)(memory_shared_address + GEMDRVEMUL_EXEC_PD);
            }
            memcpy(pexec_pd, origin, sizeof(PD));
            DPRINTF("pexec_pd->p_lowtpa: %x\n", SWAP_LONGWORD(pexec_pd->p_lowtpa));
            DPRINTF("pexec_pd->p_hitpa: %x\n", SWAP_LONGWORD(pexec_pd->p_hitpa));
            DPRINTF("pexec_pd->p_tbase: %x\n", SWAP_LONGWORD(pexec_pd->p_tbase));
            DPRINTF("pexec_pd->p_tlen: %x\n", SWAP_LONGWORD(pexec_pd->p_tlen));
            DPRINTF("pexec_pd->p_dbase: %x\n", SWAP_LONGWORD(pexec_pd->p_dbase));
            DPRINTF("pexec_pd->p_dlen: %x\n", SWAP_LONGWORD(pexec_pd->p_dlen));
            DPRINTF("pexec_pd->p_bbase: %x\n", SWAP_LONGWORD(pexec_pd->p_bbase));
            DPRINTF("pexec_pd->p_blen: %x\n", SWAP_LONGWORD(pexec_pd->p_blen));
            DPRINTF("pexec_pd->p_xdta: %x\n", SWAP_LONGWORD(pexec_pd->p_xdta));
            DPRINTF("pexec_pd->p_parent: %x\n", SWAP_LONGWORD(pexec_pd->p_parent));
            DPRINTF("pexec_pd->p_hflags: %x\n", SWAP_LONGWORD(pexec_pd->p_hflags));
            DPRINTF("pexec_pd->p_env: %x\n", SWAP_LONGWORD(pexec_pd->p_env));
            DPRINTF("pexec_pd->p_1fill\n");
            DPRINTF("pexec_pd->p_curdrv: %x\n", SWAP_LONGWORD(pexec_pd->p_curdrv));
            DPRINTF("pexec_pd->p_uftsize: %x\n", SWAP_LONGWORD(pexec_pd->p_uftsize));
            DPRINTF("pexec_pd->p_uft: %x\n", SWAP_LONGWORD(pexec_pd->p_uft));
            DPRINTF("pexec_pd->p_cmdlin: %x\n", SWAP_LONGWORD(pexec_pd->p_cmdlin));
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        case GEMDRVEMUL_SAVE_EXEC_HEADER:
        {
            payloadPtr += 6; // Skip eight words
            // Copy the from the shared memory the basepage to pexec_exec_header
            DPRINTF("Saving exec header\n");
            ExecHeader *origin = (ExecHeader *)(payloadPtr);
            // Reserve and copy the memory from origin to pexec_exec_header
            if (pexec_exec_header == NULL)
            {
                pexec_exec_header = (ExecHeader *)(memory_shared_address + GEMDRVEMUL_EXEC_HEADER);
            }
            memcpy(pexec_exec_header, origin, sizeof(ExecHeader));
            DPRINTF("pexec_exec->magic: %x\n", pexec_exec_header->magic);
            DPRINTF("pexec_exec->text: %x\n", (uint32_t)(pexec_exec_header->text_h << 16 | pexec_exec_header->text_l));
            DPRINTF("pexec_exec->data: %x\n", (uint32_t)(pexec_exec_header->data_h << 16 | pexec_exec_header->data_l));
            DPRINTF("pexec_exec->bss: %x\n", (uint32_t)(pexec_exec_header->bss_h << 16 | pexec_exec_header->bss_l));
            DPRINTF("pexec_exec->syms: %x\n", (uint32_t)(pexec_exec_header->syms_h << 16 | pexec_exec_header->syms_l));
            DPRINTF("pexec_exec->reserved1: %x\n", (uint32_t)(pexec_exec_header->reserved1_h << 16 | pexec_exec_header->reserved1_l));
            DPRINTF("pexec_exec->prgflags: %x\n", (uint32_t)(pexec_exec_header->prgflags_h << 16 | pexec_exec_header->prgflags_l));
            DPRINTF("pexec_exec->absflag: %x\n", pexec_exec_header->absflag);
            write_random_token(memory_shared_address);
            active_command_id = 0xFFFF;
            break;
        }
        default:
        {
            if (active_command_id != 0xFFFF)
            {
                DPRINTF("ERROR: Unknown command: %x\n", active_command_id);
                uint32_t d3 = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
                DPRINTF("DEBUG: %x\n", d3);
                payloadPtr += 2;
                uint32_t d4 = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
                DPRINTF("DEBUG: %x\n", d4);
                payloadPtr += 2;
                uint32_t d5 = ((uint32_t)payloadPtr[1] << 16) | payloadPtr[0];
                DPRINTF("DEBUG: %x\n", d5);
                payloadPtr += 2;
                uint8_t *payloadShowBytesPtr = (uint8_t *)payloadPtr;
                print_payload(payloadShowBytesPtr);
                write_random_token(memory_shared_address);
                active_command_id = 0xFFFF;
                break;
            }
        }
        }
// Fully bypass the print variables
#if defined(_DEBUG) && (_DEBUG != 0)
        // if (old_command != 0xFFFF)
        // {
        //     print_variables(memory_shared_address);
        // }
#endif
        // If SELECT button is pressed, launch the configurator
        bool select_pressed_now = gpio_get(SELECT_GPIO) != 0;
#if SIDETNFS_DEBUG_DUMP_ON_SELECT
        // Fase 5S: edge-triggered (rising edge only) dump of the RAM
        // diagnostic eventlog to DEBUG.TXT -- fires once per physical
        // press, before select_button_action() below (which, in the
        // non-safe-reboot case, reboots immediately and never returns).
        // This is the ONLY place DEBUG.TXT is ever written in this build;
        // no automatic main-loop/state-change/callback/Fsfirst-Fsnext write
        // exists anymore (see report).
        static bool s_diag_select_prev_pressed = false;
        if (select_pressed_now && !s_diag_select_prev_pressed)
        {
            sidetnfs_diag_dump_on_select(hd_folder);
        }
        s_diag_select_prev_pressed = select_pressed_now;
#endif
        if (select_pressed_now)
        {
            select_button_action(safe_config_reboot, write_config_only_once);
            // Write config only once to avoid hitting the flash too much
            write_config_only_once = false;
        }

        // Fase 5F/5G/5H: let the network stack process any pending packets
        // (the TNFS replies are otherwise never delivered to their callback
        // in this poll-mode build), then send the next probe step if the
        // previous one succeeded. Both touch cyw43/lwIP, so they are only
        // safe when sidetnfs_network_ok is true (WiFi/NTP actually
        // succeeded this boot) -- every other path already tore cyw43 down
        // via cyw43_arch_deinit()/network_terminate(), and polling it
        // afterwards is what previously caused a hang after ESC/no-WiFi.
        // DEBUG.TXT itself is pure FatFS and always safe to service.
        if (sidetnfs_network_ok)
        {
#if PICO_CYW43_ARCH_POLL
            cyw43_arch_poll();
#endif
            sidetnfs_probe_service();
            // Fase 5O/6B: the RAM directory-cache (and its main-loop warmup
            // tick, sidetnfs_dir_cache_service()) has been removed -- TNFS
            // directory I/O now happens synchronously inside Fsfirst/Fsnext
            // via the TNFS DTA registry (see sidetnfs_tnfs_dta_start()/
            // next() in sidetnfs_probe.c).
        }
#if SIDETNFS_USE_SD_LISTING
        // Fase 5J: one-shot SD-root scan for the DEBUG.TXT SD-vs-TNFS
        // comparison, from back when TNFS listing was new and unproven and
        // this comparison mattered for bring-up. Fase 8A: no longer called
        // under SIDETNFS_USE_TNFS_LISTING -- with TNFS long since the
        // proven, stable default (see sidetnfs_probe.h), this was the one
        // remaining unconditional FatFS/SD touch (f_opendir/f_readdir/
        // f_closedir on hd_folder, every boot) left running regardless of
        // backend, a direct violation of TNFS-mode SD isolation. Left
        // enabled only under SIDETNFS_USE_SD_LISTING, where it's harmless
        // (SD is already the live backend there) though no longer
        // meaningful either (nothing to compare against) -- kept rather
        // than deleted per this project's "don't delete, mark superseded"
        // convention; sidetnfs_scan_sd_root_if_needed() itself is
        // unchanged.
        sidetnfs_scan_sd_root_if_needed(hd_folder);
#endif
        // Fase 5S/6F: automatic per-tick DEBUG.TXT writes
        // (sidetnfs_debug_file_service()) are permanently retired -- see
        // the SELECT-button edge-handler above, which is the only place
        // DEBUG.TXT is ever written.
    }
}
