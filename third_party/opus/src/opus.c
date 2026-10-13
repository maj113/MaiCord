/* Copyright (c) 2011 Xiph.Org Foundation, Skype Limited
   Copyright (c) 2024 Arm Limited
   Written by Jean-Marc Valin and Koen Vos */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "opus.h"
#include "mathops.h"
#include "opus_private.h"


static int parse_size(const unsigned char *data, opus_int32 len, opus_int16 *size)
{
   if (len<1)
   {
      *size = -1;
      return -1;
   } else if (data[0]<252)
   {
      *size = data[0];
      return 1;
   } else if (len<2)
   {
      *size = -1;
      return -1;
   } else {
      *size = 4*data[1] + data[0];
      return 2;
   }
}

int opus_packet_get_samples_per_frame(const unsigned char *data,
      opus_int32 Fs)
{
   int audiosize;
   if (data[0]&0x80)
   {
      audiosize = ((data[0]>>3)&0x3);
      audiosize = (Fs<<audiosize)/400;
   } else if ((data[0]&0x60) == 0x60)
   {
      audiosize = (data[0]&0x08) ? Fs/50 : Fs/100;
   } else {
      audiosize = ((data[0]>>3)&0x3);
      if (audiosize == 3)
         audiosize = Fs*60/1000;
      else
         audiosize = (Fs<<audiosize)/100;
   }
   return audiosize;
}

int opus_packet_parse(const unsigned char *data, opus_int32 len,
      unsigned char *out_toc, const unsigned char *frames[48],
      opus_int16 size[48], int *payload_offset)
{
   int i, bytes;
   int count;
   int cbr;
   unsigned char ch, toc;
   int framesize;
   opus_int32 last_size;
   const unsigned char *data0 = data;

   if (size==NULL || len<0)
      return OPUS_BAD_ARG;
   if (len==0)
      return OPUS_INVALID_PACKET;

   framesize = opus_packet_get_samples_per_frame(data, 48000);

   cbr = 0;
   toc = *data++;
   len--;
   last_size = len;
   switch (toc&0x3)
   {
   /* One frame */
   case 0:
      count=1;
      break;
   /* Two CBR frames */
   case 1:
      count=2;
      cbr = 1;
      if (len&0x1)
         return OPUS_INVALID_PACKET;
      last_size = len/2;
      /* If last_size doesn't fit in size[0], we'll catch it later */
      size[0] = (opus_int16)last_size;
      break;
   /* Two VBR frames */
   case 2:
      count = 2;
      bytes = parse_size(data, len, size);
      len -= bytes;
      if (size[0]<0 || size[0] > len)
         return OPUS_INVALID_PACKET;
      data += bytes;
      last_size = len-size[0];
      break;
   /* Multiple CBR/VBR frames (from 0 to 120 ms) */
   default: /*case 3:*/
      if (len<1)
         return OPUS_INVALID_PACKET;
      /* Number of frames encoded in bits 0 to 5 */
      ch = *data++;
      count = ch&0x3F;
      if (count <= 0 || framesize*(opus_int32)count > 5760)
         return OPUS_INVALID_PACKET;
      len--;
      /* Padding flag is bit 6 */
      if (ch&0x40)
      {
         int p;
         do {
            if (len<=0)
               return OPUS_INVALID_PACKET;
            p = *data++;
            len--;
            len -= p==255 ? 254: p;
         } while (p==255);
      }
      if (len<0)
         return OPUS_INVALID_PACKET;
      /* VBR flag is bit 7 */
      cbr = !(ch&0x80);
      if (!cbr)
      {
         /* VBR case */
         last_size = len;
         for (i=0;i<count-1;i++)
         {
            bytes = parse_size(data, len, size+i);
            len -= bytes;
            if (size[i]<0 || size[i] > len)
               return OPUS_INVALID_PACKET;
            data += bytes;
            last_size -= bytes+size[i];
         }
         if (last_size<0)
            return OPUS_INVALID_PACKET;
      } else
      {
         /* CBR case */
         last_size = len/count;
         if (last_size*count!=len)
            return OPUS_INVALID_PACKET;
         for (i=0;i<count-1;i++)
            size[i] = (opus_int16)last_size;
      }
      break;
   }
   /* Because it's not encoded explicitly, it's possible the size of the
      last packet (or all the packets, for the CBR case) is larger than
      1275. Reject them here.*/
   if (last_size > 1275)
      return OPUS_INVALID_PACKET;
   size[count-1] = (opus_int16)last_size;

   if (payload_offset)
      *payload_offset = (int)(data-data0);

   for (i=0;i<count;i++)
   {
      if (frames)
         frames[i] = data;
      data += size[i];
   }

   if (out_toc)
      *out_toc = toc;

   return count;
}
