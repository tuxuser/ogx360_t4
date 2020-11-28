#include <Arduino.h>
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "xid_xmu.h"
#include "printf.h"

#if (XID_XMU >= 1)

#if defined(USE_EXT_FLASH) && !defined(__IMXRT1062__)
#error To use external flash, you must be using a Teensy4 or 4.1
#endif

#if defined(__IMXRT1062__) && defined(USE_EXT_FLASH)
static int32_t flash_read(uint32_t block, uint32_t offset, void *buf, uint32_t size);
static int32_t flash_program(uint32_t block, uint32_t offset, const void *buf, uint32_t size);
static int32_t flash_erase(uint32_t block);
static int32_t flash_wait(uint32_t microseconds);
#elif defined(__IMXRT1062__) && defined(USE_BUILTIN_SDCARD)
static uint32_t sdcard_block_count();
static uint16_t sdcard_block_size();
static void sdcard_read(uint32_t block, uint32_t offset, void *buf, uint32_t size);
static void sdcard_write(uint32_t block, uint32_t offset, const void *buf, uint32_t size);
#else
#ifndef DMAMEM
#define DMAMEM
#endif
static DMAMEM uint8_t msc_ram_disk[MSC_BLOCK_SIZE * MSC_BLOCK_NUM];
#endif

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&XMU_DESC_DEVICE;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return XMU_DESC_CONFIGURATION;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)index;
    (void)langid;

    return NULL;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    TU_LOG2("READ10: lba: %i, off: %i, len: %i\r\n", lba, offset, bufsize);
#if defined(__IMXRT1062__) && defined(USE_EXT_FLASH)
    flash_read(lba, offset, buffer, bufsize);
#elif defined(__IMXRT1062__) && defined(USE_BUILTIN_SDCARD)
    sdcard_read(lba, offset, buffer, bufsize);
#else
    memcpy(buffer, &msc_ram_disk[lba * MSC_BLOCK_SIZE] + offset, bufsize);
#endif

    return bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    TU_LOG2("WRITE10: lba: %i, off: %i, len: %i\r\n", lba, offset, bufsize);
#if defined(__IMXRT1062__) && defined(USE_EXT_FLASH)
    flash_erase(lba);
    flash_program(lba, offset, buffer, bufsize);
#elif defined(__IMXRT1062__) && defined(USE_BUILTIN_SDCARD)
    sdcard_write(lba, offset, buffer, bufsize);
#else
    memcpy(&msc_ram_disk[lba * MSC_BLOCK_SIZE] + offset, buffer, bufsize);
#endif

    return bufsize;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    TU_LOG2("tud_msc_capacity_cb\r\n");
#if defined(__IMXRT1062__) && defined(USE_BUILTIN_SDCARD)
    *block_count = sdcard_block_count();
    *block_size = sdcard_block_size();
#else
    *block_count = MSC_BLOCK_NUM;
    *block_size = MSC_BLOCK_SIZE;
#endif
}

//OG Xbox never seems to call this SCSI command
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    TU_LOG2("tud_msc_test_unit_ready_cb\r\n");
    return true;
}

//OG Xbox never seems to call this SCSI command
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    TU_LOG2("SCSI_CMD_INQUIRY\r\n");

    const char vid[] = "Ryzee119";
    const char pid[] = "Mass Storage";
    const char rev[] = "1.0";

    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
    return;
}

// Callback invoked when received an SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    scsi_cmd_type_t scsi = scsi_cmd[0];
    (void)scsi;
    TU_LOG2("tud_msc_scsi_cb : cmd: %02x\r\n", scsi);
    return -1;
}

#if defined(__IMXRT1062__) && defined(USE_EXT_FLASH)

#define LUT0(opcode, pads, operand) (FLEXSPI_LUT_INSTRUCTION((opcode), (pads), (operand)))
#define LUT1(opcode, pads, operand) (FLEXSPI_LUT_INSTRUCTION((opcode), (pads), (operand)) << 16)
#define CMD_SDR FLEXSPI_LUT_OPCODE_CMD_SDR
#define ADDR_SDR FLEXSPI_LUT_OPCODE_RADDR_SDR
#define READ_SDR FLEXSPI_LUT_OPCODE_READ_SDR
#define WRITE_SDR FLEXSPI_LUT_OPCODE_WRITE_SDR
#define DUMMY_SDR FLEXSPI_LUT_OPCODE_DUMMY_SDR
#define PINS1 FLEXSPI_LUT_NUM_PADS_1
#define PINS4 FLEXSPI_LUT_NUM_PADS_4

static void flexspi2_ip_command(uint32_t index, uint32_t addr)
{
    uint32_t n;
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(index);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    while (!((n = FLEXSPI2_INTR) & FLEXSPI_INTR_IPCMDDONE));
    if (n & FLEXSPI_INTR_IPCMDERR)
    {
        FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDERR;
    }
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

static void flexspi2_ip_read(uint32_t index, uint32_t addr, void *data, uint32_t length)
{
    uint8_t *p = (uint8_t *)data;

    FLEXSPI2_INTR = FLEXSPI_INTR_IPRXWA;
    // Clear RX FIFO and set watermark to 16 bytes
    FLEXSPI2_IPRXFCR = FLEXSPI_IPRXFCR_CLRIPRXF | FLEXSPI_IPRXFCR_RXWMRK(1);
    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(index) | FLEXSPI_IPCR1_IDATSZ(length);
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    // page 1649 : Reading Data from IP RX FIFO
    // page 1706 : Interrupt Register (INTR)
    // page 1723 : IP RX FIFO Control Register (IPRXFCR)
    // page 1732 : IP RX FIFO Status Register (IPRXFSTS)

    while (1)
    {
        if (length >= 16)
        {
            if (FLEXSPI2_INTR & FLEXSPI_INTR_IPRXWA)
            {
                volatile uint32_t *fifo = &FLEXSPI2_RFDR0;
                uint32_t a = *fifo++;
                uint32_t b = *fifo++;
                uint32_t c = *fifo++;
                uint32_t d = *fifo++;
                *(uint32_t *)(p + 0) = a;
                *(uint32_t *)(p + 4) = b;
                *(uint32_t *)(p + 8) = c;
                *(uint32_t *)(p + 12) = d;
                p += 16;
                length -= 16;
                FLEXSPI2_INTR = FLEXSPI_INTR_IPRXWA;
            }
        }
        else if (length > 0)
        {
            if ((FLEXSPI2_IPRXFSTS & 0xFF) >= ((length + 7) >> 3))
            {
                volatile uint32_t *fifo = &FLEXSPI2_RFDR0;
                while (length >= 4)
                {
                    *(uint32_t *)(p) = *fifo++;
                    p += 4;
                    length -= 4;
                }
                uint32_t a = *fifo;
                if (length >= 1)
                {
                    *p++ = a & 0xFF;
                    a = a >> 8;
                }
                if (length >= 2)
                {
                    *p++ = a & 0xFF;
                    a = a >> 8;
                }
                if (length >= 3)
                {
                    *p++ = a & 0xFF;
                    a = a >> 8;
                }
                length = 0;
            }
        }
        else
        {
            if (FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDDONE)
                break;
        }
    }
    if (FLEXSPI2_INTR & FLEXSPI_INTR_IPCMDERR)
    {
        FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDERR;
    }
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

static void flexspi2_ip_write(uint32_t index, uint32_t addr, const void *data, uint32_t length)
{
    const uint8_t *src;
    uint32_t n, wrlen;

    FLEXSPI2_IPCR0 = addr;
    FLEXSPI2_IPCR1 = FLEXSPI_IPCR1_ISEQID(index) | FLEXSPI_IPCR1_IDATSZ(length);
    src = (const uint8_t *)data;
    FLEXSPI2_IPCMD = FLEXSPI_IPCMD_TRG;
    while (!((n = FLEXSPI2_INTR) & FLEXSPI_INTR_IPCMDDONE))
    {
        if (n & FLEXSPI_INTR_IPTXWE)
        {
            wrlen = length;
            if (wrlen > 8)
                wrlen = 8;
            if (wrlen > 0)
            {
                memcpy((void *)&FLEXSPI2_TFDR0, src, wrlen);
                src += wrlen;
                length -= wrlen;
                FLEXSPI2_INTR = FLEXSPI_INTR_IPTXWE;
            }
        }
    }
    if (n & FLEXSPI_INTR_IPCMDERR)
    {
        FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDERR;
    }
    FLEXSPI2_INTR = FLEXSPI_INTR_IPCMDDONE;
}

static int32_t flash_erase(uint32_t block)
{
    flexspi2_ip_command(10, 0x00800000);
    const uint32_t addr = block * MSC_BLOCK_SIZE;
    flexspi2_ip_command(12, 0x00800000 + addr);
    flash_wait(0);
    return 0;
}

static int32_t flash_program(uint32_t block, uint32_t offset, const void *buf, uint32_t size)
{

    uint32_t addr = block * MSC_BLOCK_SIZE + offset;
    int32_t s = size;
    while (s > 0)
    {
        int b;
        (s < PAGE_SIZE) ? (b = size) : (b = PAGE_SIZE);
        flexspi2_ip_command(10, 0x00800000);
        flexspi2_ip_write(11, 0x00800000 + addr, buf, b);
        buf += b;
        addr += b;
        s -= b;
        flash_wait(0);
    }

    return 0;
}

static int32_t flash_read(uint32_t block, uint32_t offset, void *buf, uint32_t size)
{
    const uint32_t addr = block * MSC_BLOCK_SIZE + offset;
    flexspi2_ip_read(9, 0x00800000 + addr, buf, size);
    return 0;
}

static int32_t flash_wait(uint32_t timeout)
{
    (void) timeout;
    while (1)
    {
        uint8_t status;
        flexspi2_ip_read(13, 0x00800000, &status, 1);
        if (!(status & 1))
            break;
        yield();
    }
    return 0;
}

bool flash_init()
{
    uint8_t buf[4] = {0, 0, 0, 0};

    FLEXSPI2_LUTKEY = FLEXSPI_LUTKEY_VALUE;
    FLEXSPI2_LUTCR = FLEXSPI_LUTCR_UNLOCK;
    // cmd index 8 = read ID bytes
    FLEXSPI2_LUT32 = LUT0(CMD_SDR, PINS1, 0x9F) | LUT1(READ_SDR, PINS1, 1);
    FLEXSPI2_LUT33 = 0;

    flexspi2_ip_read(8, 0x00800000, buf, 3);

    TU_LOG1("Flash ID: %02X %02X %02X\r\n", buf[0], buf[1], buf[2]);

    // configure FlexSPI2 for chip's size
    FLEXSPI2_FLSHA2CR0 = FLASH_CHIP_SIZE / 1024;

    FLEXSPI2_LUTKEY = FLEXSPI_LUTKEY_VALUE;
    FLEXSPI2_LUTCR = FLEXSPI_LUTCR_UNLOCK;
    // cmd index 9 = read QSPI
    FLEXSPI2_LUT36 = LUT0(CMD_SDR, PINS1, 0x6B) | LUT1(ADDR_SDR, PINS1, 24);
    FLEXSPI2_LUT37 = LUT0(DUMMY_SDR, PINS4, 8) | LUT1(READ_SDR, PINS4, 1);
    FLEXSPI2_LUT38 = 0;
    // cmd index 10 = write enable
    FLEXSPI2_LUT40 = LUT0(CMD_SDR, PINS1, 0x06);
    // cmd index 11 = program QSPI
    FLEXSPI2_LUT44 = LUT0(CMD_SDR, PINS1, 0x32) | LUT1(ADDR_SDR, PINS1, 24);
    FLEXSPI2_LUT45 = LUT0(WRITE_SDR, PINS4, 1);
    // cmd index 12 = sector erase
    FLEXSPI2_LUT48 = LUT0(CMD_SDR, PINS1, 0x20) | LUT1(ADDR_SDR, PINS1, 24);
    FLEXSPI2_LUT49 = 0;
    // cmd index 13 = get status 1 register
    FLEXSPI2_LUT52 = LUT0(CMD_SDR, PINS1, 0x05) | LUT1(READ_SDR, PINS1, 1);
    FLEXSPI2_LUT53 = 0;
    // cmd index 14 = get status 2 register
    FLEXSPI2_LUT56 = LUT0(CMD_SDR, PINS1, 0x35) | LUT1(READ_SDR, PINS1, 1);
    FLEXSPI2_LUT57 = 0;
    // cmd index 15 = write status 2 register
    FLEXSPI2_LUT60 = LUT0(CMD_SDR, PINS1, 0x31) | LUT1(CMD_SDR, PINS1, 0x02);
    FLEXSPI2_LUT61 = 0;

    //Write Enable
    flexspi2_ip_command(10, 0x00800000);
    //Write QE bit in status reg 2 in case it isnt set at factory
    flexspi2_ip_command(15, 0x00800000);

    return true;
}
#elif defined(__IMXRT1062__) && defined(USE_BUILTIN_SDCARD)

#include "diskio_wrapper.h"

static uint32_t sdcard_block_count()
{
    return _disk_volume_num_blocks();
}

static uint16_t sdcard_block_size()
{
    return (uint16_t)_disk_volume_get_block_size();
}

static void sdcard_read(uint32_t block, uint32_t offset, void *buf, uint32_t size)
{
    uint32_t pos = 0;
    uint32_t curr_block = block;
    uint32_t block_sz = sdcard_block_size();
    uint8_t block_buf[block_sz];

    if (offset > block_sz)
    {
        TU_LOG2("sdcard_read: Offset exceeds block size")
        return;
    }

    /*
     * Read first (partial) block
     */

    // Calculate size to copy, either to the end of block or partial of that
    uint32_t copy_size = min((block_sz - offset), size);
    TU_LOG2("sdcard_read: Reading initial block %04X, offset: %04X, size: %04X",
            curr_block, offset, copy_size);
    _read_block(curr_block, block_buf);
    memcpy(buf, &block_buf[offset], copy_size);
    pos += copy_size;
    curr_block++;

    /*
     * Read rest of data
     */
    while (pos < size)
    {
        copy_size = min(block_sz, (size - pos));
        TU_LOG2("sdcard_read: Reading successive block %04X, offset: %04X, size: %04X",
                curr_block, offset, copy_size);
        _read_block(curr_block, block_buf);
        memcpy(&buf[pos], block_buf, copy_size);
        pos += copy_size;
        curr_block++;
    }
}

static void sdcard_write(uint32_t block, uint32_t offset, const void *buf, uint32_t size)
{
    uint32_t pos = 0;
    uint32_t curr_block = block;
    uint32_t block_sz = sdcard_block_size();
    uint8_t block_buf[block_sz];

    if (offset > block_sz)
    {
        TU_LOG2("sdcard_write: Offset exceeds block size")
        return;
    }

    /*
     * Read first (partial) block
     */

    // Calculate size to copy, either to the end of block or partial of that
    uint32_t copy_size = min((block_sz - offset), size);
    TU_LOG2("sdcard_write: Reading initial block %04X, offset: %04X, size: %04X",
            curr_block, offset, copy_size);
    _read_block(curr_block, block_buf);
    // Copying (partitial) data to full block data
    memcpy(&block_buf[offset], buf, copy_size);
    // Writing full block back to sdcard
    _write_block(curr_block, block_buf);

    pos += copy_size;
    curr_block++;

    /*
     * Read rest of data
     */
    while (pos < size)
    {
        copy_size = min(block_sz, (size - pos));
        if (copy_size < block_sz)
        {
            // Partitial block write
            TU_LOG2("sdcard_write: Partial write on block %04X, size: %04X",
                    curr_block, copy_size);
            _read_block(curr_block, block_buf);
            memcpy(&block_buf, &buf[pos], copy_size);
            _write_block(curr_block, block_buf);
        }
        else
        {
            // Full block write
            TU_LOG2("sdcard_write: Full write on block %04X, size: %04X",
                    curr_block, copy_size);
            _write_block(curr_block, &buf[pos]);
        }

        pos += copy_size;
        curr_block++;
    }
}

bool sdcard_init()
{
    _disk_init();
    return true;
}

#endif

#endif