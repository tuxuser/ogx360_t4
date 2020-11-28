// Copyright 2020, Ryan Wendland, usb64
// SPDX-License-Identifier: MIT

#ifndef DISKIO_WRAPPER_H_
#define DISKIO_WRAPPER_H_

#ifdef __cplusplus
extern "C"
{
#endif

#include <Arduino.h>

    void _disk_init(void);
    uint32_t _disk_volume_num_blocks();
    uint32_t _disk_volume_get_block_size();
    uint32_t _disk_volume_get_cluster_size();
    uint8_t _read_block(uint32_t block, uint8_t *buf);
    uint8_t _write_block(uint32_t block, const void *buf);

#ifdef __cplusplus
}
#endif
#endif /* DISKIO_WRAPPER_H_ */