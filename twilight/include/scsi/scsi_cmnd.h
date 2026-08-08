#pragma once

struct scsi_cmnd {
    void *request_buffer;
    unsigned int request_bufflen;
};
