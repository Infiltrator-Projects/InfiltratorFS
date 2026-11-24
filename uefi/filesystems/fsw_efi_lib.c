/**
 * \file fsw_efi_lib.c
 * EFI host environment library functions.
 */

/*-
 * Copyright (c) 2006 Christoph Pfisterer
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *  * Neither the name of Christoph Pfisterer nor the names of the
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "fsw_efi.h"


//
// time conversion
//
// Adopted from public domain code in FreeBSD libc.
//

#define SECSPERMIN      60
#define MINSPERHOUR     60
#define HOURSPERDAY     24
#define DAYSPERWEEK     7
#define DAYSPERNYEAR    365
#define DAYSPERLYEAR    366
#define SECSPERHOUR     (SECSPERMIN * MINSPERHOUR)
#define SECSPERDAY      ((long) SECSPERHOUR * HOURSPERDAY)
#define MONSPERYEAR     12

#define EPOCH_YEAR      1970
#define EPOCH_WDAY      TM_THURSDAY

#define isleap(y) (((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))
#define LEAPS_THRU_END_OF(y)    ((y) / 4 - (y) / 100 + (y) / 400)

static const int mon_lengths[2][MONSPERYEAR] = {
    { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
    { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};
static const int year_lengths[2] = {
    DAYSPERNYEAR, DAYSPERLYEAR
};

VOID fsw_efi_decode_time(OUT EFI_TIME *EfiTime, IN UINT32 UnixTime)
{
    long        days, rem;
    int         y, newy, yleap;
    const int   *ip;

    ZeroMem(EfiTime, sizeof(EFI_TIME));

    days = UnixTime / SECSPERDAY;
    rem = UnixTime % SECSPERDAY;

    EfiTime->Hour = (UINT8) (rem / SECSPERHOUR);
    rem = rem % SECSPERHOUR;
    EfiTime->Minute = (UINT8) (rem / SECSPERMIN);
    EfiTime->Second = (UINT8) (rem % SECSPERMIN);

    y = EPOCH_YEAR;
    while (days < 0 || days >= (long) year_lengths[yleap = isleap(y)]) {
        newy = y + days / DAYSPERNYEAR;
        if (days < 0)
            --newy;
        days -= (newy - y) * DAYSPERNYEAR +
            LEAPS_THRU_END_OF(newy - 1) -
            LEAPS_THRU_END_OF(y - 1);
        y = newy;
    }
    EfiTime->Year = (UINT16)y;
    ip = mon_lengths[yleap];
    for (EfiTime->Month = 0; days >= (long) ip[EfiTime->Month]; ++(EfiTime->Month))
        days = days - (long) ip[EfiTime->Month];
    EfiTime->Month++;  // adjust range to EFI conventions
    EfiTime->Day = (UINT8) (days + 1);
}

//
// String functions, used for file and volume info
//

/*
 * The wrapper's length and byte count are signed ints from filesystem
 * metadata. An EMPTY string may have invalid remaining members. Reject
 * negative or inconsistent counts before converting them to EFI UINTN;
 * size the terminator using UINTN arithmetic rather than signed addition.
 * Non-UTF16 encodings have no conversion in this EFI adapter.
 */
static BOOLEAN fsw_efi_valid_utf16(const struct fsw_string *s)
{
    return s != NULL && s->type == FSW_STRING_TYPE_UTF16 &&
           s->len >= 0 && s->size >= 0 &&
           (UINTN)s->size % sizeof(CHAR16) == 0 &&
           (UINTN)s->len == (UINTN)s->size / sizeof(CHAR16) &&
           (s->size == 0 || s->data != NULL);
}

UINTN fsw_efi_strsize(struct fsw_string *s)
{
    if (!fsw_efi_valid_utf16(s))
        return sizeof(CHAR16);
    return (UINTN)s->size + sizeof(CHAR16);
}

/*
 * DestSize is the actual available byte capacity of the destination.
 * Invalid strings are represented by an empty UTF-16 string, as before
 * for unsupported encodings. Never read data or write beyond that
 * capacity; a too-small destination is left empty if a terminator fits.
 */
VOID fsw_efi_strcpy(CHAR16 *Dest, UINTN DestSize, struct fsw_string *src)
{
    UINTN bytes;

    if (Dest == NULL || DestSize < sizeof(CHAR16))
        return;
    Dest[0] = 0;
    if (!fsw_efi_valid_utf16(src))
        return;
    bytes = (UINTN)src->size;
    if (bytes > DestSize - sizeof(CHAR16))
        return;
    if (bytes != 0)
        refit_call3_wrapper(gBS->CopyMem, Dest, src->data, bytes);
    Dest[bytes / sizeof(CHAR16)] = 0;
}

// EOF
