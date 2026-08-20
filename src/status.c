// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/status.h"

const char *infs_status_string(infs_status status)
{
    switch (status) {
    case INFS_STATUS_OK: return "success";
    case INFS_STATUS_ERROR: return "unspecified filesystem error";
    case INFS_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case INFS_STATUS_IO_ERROR: return "storage input/output error";
    case INFS_STATUS_CORRUPT: return "filesystem corruption detected";
    case INFS_STATUS_READ_ONLY: return "filesystem is read-only";
    case INFS_STATUS_NO_SPACE: return "no space available";
    case INFS_STATUS_NOT_FOUND: return "object not found";
    case INFS_STATUS_ALREADY_EXISTS: return "object already exists";
    case INFS_STATUS_NOT_DIRECTORY: return "object is not a directory";
    case INFS_STATUS_IS_DIRECTORY: return "object is a directory";
    case INFS_STATUS_NOT_EMPTY: return "directory is not empty";
    case INFS_STATUS_NAME_TOO_LONG: return "name is too long";
    case INFS_STATUS_FILE_TOO_LARGE: return "file is too large";
    case INFS_STATUS_OVERFLOW: return "numeric overflow";
    case INFS_STATUS_LOOP_DETECTED: return "metadata loop detected";
    case INFS_STATUS_NOT_SUPPORTED: return "operation or format is not supported";
    case INFS_STATUS_NO_MEMORY: return "not enough memory";
    case INFS_STATUS_INTERRUPTED: return "operation was interrupted";
    case INFS_STATUS_BUSY: return "storage target is busy";
    default: return "unknown filesystem status";
    }
}
