// Copyright 2020, Ryan Wendland, usb64
// SPDX-License-Identifier: MIT

#include <SD.h>
#include <SPI.h>
#include "diskio_wrapper.h"

Sd2Card card;
SdVolume volume;

void _disk_init(void)
{
    SD.begin(BUILTIN_SDCARD);
    card.init(SPI_HALF_SPEED, BUILTIN_SDCARD);
    volume.init(card);
}

uint32_t _disk_volume_num_blocks()
{
    return volume.blocksPerCluster() * volume.clusterCount();
}

uint32_t _disk_volume_get_cluster_size()
{
    return volume.blocksPerCluster();
}

uint32_t _disk_volume_get_block_size()
{
    return 512;
}

uint8_t _read_block(uint32_t block, uint8_t *buf)
{
    return card.readBlock(block, buf);
}

uint8_t _write_block(uint32_t block, const uint8_t *buf)
{
    return card.writeBlock(block, buf);
}