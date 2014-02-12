/* Copyright (C) 2000-2003 Constantin Kaplinsky.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
 * Copyright 2014 Pierre Ossman for Cendio AB.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

//
// tightEncode.h - Tight encoding function.
//
// This file is #included after having set the following macro:
// BPP                - 8, 16 or 32
//

#include <assert.h>

namespace rfb {

// CONCAT2E concatenates its arguments, expanding them if they are macros

#ifndef CONCAT2E
#define CONCAT2(a,b) a##b
#define CONCAT2E(a,b) CONCAT2(a,b)
#endif

#define PIXEL_T rdr::CONCAT2E(U,BPP)
#define TIGHT_ENCODE TightEncoder::CONCAT2E(tightEncode,BPP)
#define HASH_FUNCTION CONCAT2E(HASH_FUNC,BPP)
#define PACK_PIXELS TightEncoder::CONCAT2E(packPixels,BPP)
#define ENCODE_SOLID_RECT TightEncoder::CONCAT2E(encodeSolidRect,BPP)
#define ENCODE_FULLCOLOR_RECT TightEncoder::CONCAT2E(encodeFullColorRect,BPP)
#define ENCODE_MONO_RECT TightEncoder::CONCAT2E(encodeMonoRect,BPP)
#define ENCODE_INDEXED_RECT TightEncoder::CONCAT2E(encodeIndexedRect,BPP)
#define ENCODE_JPEG_RECT TightEncoder::CONCAT2E(encodeJpegRect,BPP)
#define FAST_FILL_PALETTE TightEncoder::CONCAT2E(fastFillPalette,BPP)
#define FILL_PALETTE TightEncoder::CONCAT2E(fillPalette,BPP)
#define CHECK_SOLID_TILE TightEncoder::CONCAT2E(checkSolidTile,BPP)

#ifndef TIGHT_ONCE
#define TIGHT_ONCE

//
// Compress the data (but do not perform actual compression if the data
// size is less than TIGHT_MIN_TO_COMPRESS bytes.
//

void TightEncoder::compressData(const void *buf, unsigned int length,
                                rdr::ZlibOutStream *zos, int zlibLevel,
                                rdr::OutStream *os)
{
  if (length < TIGHT_MIN_TO_COMPRESS) {
    os->writeBytes(buf, length);
  } else {
    // FIXME: Using a temporary MemOutStream may be not efficient.
    //        Maybe use the same static object used in the JPEG coder?
    int maxBeforeSize = pconf->maxRectSize * (clientpf.bpp / 8);
    int maxAfterSize = maxBeforeSize + (maxBeforeSize + 99) / 100 + 12;
    rdr::MemOutStream mem_os(maxAfterSize);
    zos->setUnderlying(&mem_os);
    zos->setCompressionLevel(zlibLevel);
    zos->writeBytes(buf, length);
    zos->flush();
    zos->setUnderlying(NULL);
    writeCompact(os, mem_os.length());
    os->writeBytes(mem_os.data(), mem_os.length());
  }
}

#endif  // #ifndef TIGHT_ONCE

//
// Convert 32-bit color samples into 24-bit colors, in place.
// Performs packing only when redMax, greenMax and blueMax are all 255.
// Color components are assumed to be byte-aligned.
//

unsigned int PACK_PIXELS (PIXEL_T *buf, unsigned int count)
{
#if (BPP != 32)
  return count * sizeof(PIXEL_T);
#else
  if (!pack24)
    return count * sizeof(PIXEL_T);

  rdr::U32 pix;
  rdr::U8 *dst = (rdr::U8 *)buf;
  for (unsigned int i = 0; i < count; i++) {
    pix = *buf++;
    clientpf.rgbFromBuffer(dst, (rdr::U8*)&pix, 1, NULL);
    dst += 3;
  }
  return count * 3;
#endif
}

//
// Main function of the Tight encoder
//

void TIGHT_ENCODE (const Rect& r, rdr::OutStream *os, bool forceSolid)
{
  int stride;
  rdr::U32 solidColor;
  const PIXEL_T *rawPixels = (const PIXEL_T *)ig->getRawBufferR(r, &stride);
  PIXEL_T *pixels = NULL;
  bool grayScaleJPEG = (jpegSubsampling == subsampleGray && jpegQuality != -1);

#if (BPP == 32)
  // Check if it's necessary to pack 24-bit pixels, and
  // compute appropriate shift values if necessary.
  pack24 = clientpf.is888();
#endif

  if (forceSolid) {
    // Subrectangle has already been determined to be solid.
    ig->translatePixels(rawPixels, &solidColor, 1);
    pixels = (PIXEL_T *)&solidColor;
    palette.clear();
    palette.insert(solidColor, 1);
  } else {
    // Analyze subrectangle's colors to determine best encoding method.
    palMaxColors = r.area() / pconf->idxMaxColorsDivisor;
    if (jpegQuality != -1)
      palMaxColors = pconf->palMaxColorsWithJPEG;
    if (palMaxColors < 2 && r.area() >= pconf->monoMinRectSize)
      palMaxColors = 2;

    if (clientpf.equal(serverpf) && clientpf.bpp >= 16) {
      // Count the colors in the raw buffer, so we can avoid unnecessary pixel
      // translation when encoding with JPEG.
      if (grayScaleJPEG) palette.clear();
      else FAST_FILL_PALETTE(rawPixels, stride, r);

      // JPEG can read from the raw buffer, but for the other methods, we need
      // to translate the raw pixels into an intermediate buffer.
      if(palette.size() != 0 || jpegQuality == -1) {
        pixels = (PIXEL_T *)writer->getImageBuf(r.area());
        stride = r.width();
        ig->getImage(pixels, r);
      }
    } else {
      // Pixel translation will be required, so create an intermediate buffer,
      // translate the raw pixels into it, and count its colors.
      pixels = (PIXEL_T *)writer->getImageBuf(r.area());
      stride = r.width();
      ig->getImage(pixels, r);

      if (grayScaleJPEG) palette.clear();
      else FILL_PALETTE(pixels, r.area());
    }
  }

  switch (palette.size()) {
  case 0:
    // Truecolor image
#if (BPP != 8)
    if (jpegQuality != -1) {
      if (pixels)
        ENCODE_JPEG_RECT(pixels, stride, r, os);
      else
        ENCODE_JPEG_RECT((PIXEL_T *)rawPixels, stride, r, os);
      break;
    }
#endif
    ENCODE_FULLCOLOR_RECT(pixels, r, os);
    break;
  case 1:
    // Solid rectangle
    ENCODE_SOLID_RECT(pixels, os);
    break;
  case 2:
    // Two-color rectangle
    ENCODE_MONO_RECT(pixels, r, os);
    break;
#if (BPP != 8)
  default:
    // Up to 256 different colors
    ENCODE_INDEXED_RECT(pixels, r, os);
#endif
  }
}

//
// Subencoding implementations.
//

void ENCODE_SOLID_RECT (PIXEL_T *buf, rdr::OutStream *os)
{
  os->writeU8(0x08 << 4);

  int length = PACK_PIXELS(buf, 1);
  os->writeBytes(buf, length);
}

void ENCODE_FULLCOLOR_RECT (PIXEL_T *buf, const Rect& r, rdr::OutStream *os)
{
  const int streamId = 0;
  os->writeU8(streamId << 4);

  int length = PACK_PIXELS(buf, r.area());
  compressData(buf, length, &zos[streamId], pconf->rawZlibLevel, os);
}

void ENCODE_MONO_RECT (PIXEL_T *buf, const Rect& r, rdr::OutStream *os)
{
  const int streamId = 1;
  os->writeU8((streamId | 0x04) << 4);
  os->writeU8(0x01);

  // Write the palette
  PIXEL_T pal[2] = { (PIXEL_T)palette.getColour(0),
                     (PIXEL_T)palette.getColour(1) };
  os->writeU8(1);
  os->writeBytes(pal, PACK_PIXELS(pal, 2));

  // Encode the data in-place
  PIXEL_T *src = buf;
  rdr::U8 *dst = (rdr::U8 *)buf;
  int w = r.width();
  int h = r.height();
  PIXEL_T bg;
  unsigned int value, mask;
  int aligned_width;
  int x, y, bg_bits;

  bg = (PIXEL_T) pal[0];
  aligned_width = w - w % 8;

  for (y = 0; y < h; y++) {
    for (x = 0; x < aligned_width; x += 8) {
      for (bg_bits = 0; bg_bits < 8; bg_bits++) {
        if (*src++ != bg)
          break;
      }
      if (bg_bits == 8) {
        *dst++ = 0;
        continue;
      }
      mask = 0x80 >> bg_bits;
      value = mask;
      for (bg_bits++; bg_bits < 8; bg_bits++) {
        mask >>= 1;
        if (*src++ != bg) {
          value |= mask;
        }
      }
      *dst++ = (rdr::U8)value;
    }

    mask = 0x80;
    value = 0;
    if (x >= w)
      continue;

    for (; x < w; x++) {
      if (*src++ != bg) {
        value |= mask;
      }
      mask >>= 1;
    }
    *dst++ = (rdr::U8)value;
  }

  // Write the data
  int length = (w + 7) / 8;
  length *= h;
  compressData(buf, length, &zos[streamId], pconf->monoZlibLevel, os);
}

#if (BPP != 8)
void ENCODE_INDEXED_RECT (PIXEL_T *buf, const Rect& r, rdr::OutStream *os)
{
  const int streamId = 2;
  os->writeU8((streamId | 0x04) << 4);
  os->writeU8(0x01);

  // Write the palette
  {
    PIXEL_T pal[256];
    for (int i = 0; i < palette.size(); i++)
      pal[i] = (PIXEL_T)palette.getColour(i);
    os->writeU8((rdr::U8)(palette.size() - 1));
    os->writeBytes(pal, PACK_PIXELS(pal, palette.size()));
  }

  // Encode data in-place
  PIXEL_T *src = buf;
  rdr::U8 *dst = (rdr::U8 *)buf;
  int count = r.area();
  PIXEL_T rgb;
  int rep = 0;
  unsigned char idx;

  while (count--) {
    rgb = *src++;
    while (count && *src == rgb) {
      rep++, src++, count--;
    }
    idx = palette.lookup(rgb);
    *dst++ = idx;
    while (rep) {
      *dst++ = idx;
      rep--;
    }
  }

  // Write the data
  compressData(buf, r.area(), &zos[streamId], pconf->idxZlibLevel, os);
}
#endif  // #if (BPP != 8)

//
// JPEG compression.
//

#if (BPP != 8)
void ENCODE_JPEG_RECT (PIXEL_T *buf, int stride, const Rect& r,
                       rdr::OutStream *os)
{
  jc.clear();
  jc.compress((rdr::U8 *)buf, stride, r, clientpf,
    jpegQuality, jpegSubsampling);
  os->writeU8(0x09 << 4);
  writeCompact(os, jc.length());
  os->writeBytes(jc.data(), jc.length());
}
#endif  // #if (BPP != 8)

//
// Determine the number of colors in the rectangle, and fill in the palette.
//

#if (BPP == 8)

void FILL_PALETTE (PIXEL_T *data, int count)
{
  PIXEL_T c0, c1;
  int i, n0, n1;

  palette.clear();

  c0 = data[0];
  for (i = 1; i < count && data[i] == c0; i++);
  if (i == count) {
    palette.insert(c0, i);
    return;                       // Solid rectangle
  }

  if (palMaxColors < 2)
    return;

  n0 = i;
  c1 = data[i];
  n1 = 0;
  for (i++; i < count; i++) {
    if (data[i] == c0) {
      n0++;
    } else if (data[i] == c1) {
      n1++;
    } else
      break;
  }
  if (i == count) {
    palette.insert(c0, n0);     // Two colors
    palette.insert(c1, n1);
  }
}

void FAST_FILL_PALETTE (const PIXEL_T *data, int stride, const Rect& r)
{
}

#else   // (BPP != 8)

void FILL_PALETTE (PIXEL_T *data, int count)
{
  PIXEL_T c0, c1, ci = 0;
  int i, n0, n1, ni;

  palette.clear();

  c0 = data[0];
  for (i = 1; i < count && data[i] == c0; i++);
  if (i >= count) {
    palette.insert(c0, i);      // Solid rectangle
    return;
  }

  if (palMaxColors < 2)
    return;                     // Full-color format preferred

  n0 = i;
  c1 = data[i];
  n1 = 0;
  for (i++; i < count; i++) {
    ci = data[i];
    if (ci == c0) {
      n0++;
    } else if (ci == c1) {
      n1++;
    } else
      break;
  }
  palette.insert(c0, n0);
  palette.insert(c1, n1);
  if (i >= count)
    return;                     // Two colors

  ni = 1;
  for (i++; i < count; i++) {
    if (data[i] == ci) {
      ni++;
    } else {
      if (!palette.insert (ci, ni) || (palette.size() > palMaxColors)) {
        palette.clear();
        return;
      }
      ci = data[i];
      ni = 1;
    }
  }
  if (!palette.insert (ci, ni) || (palette.size() > palMaxColors))
    palette.clear();
}

void FAST_FILL_PALETTE (const PIXEL_T *data, int stride, const Rect& r)
{
  PIXEL_T c0, c1, ci = 0, mask, c0t, c1t, cit;
  int n0, n1, ni;
  int w = r.width(), h = r.height();
  const PIXEL_T *rowptr, *colptr, *rowptr2, *colptr2,
    *dataend = &data[stride * h];
  bool willTransform = ig->willTransform();

  serverpf.bufferFromPixel((rdr::U8*)&mask, ~0);

  palette.clear();

  c0 = data[0] & mask;
  n0 = 0;
  for (rowptr = data; rowptr < dataend; rowptr += stride) {
    for (colptr = rowptr; colptr < &rowptr[w]; colptr++) {
      if (((*colptr) & mask) != c0)
        goto soliddone;
      n0++;
    }
  }

  soliddone:
  if (rowptr >= dataend) {
    palette.insert(c0, 1);      // Solid rectangle
    return;
  }
  if (palMaxColors < 2)
    return;                     // Full-color format preferred

  c1 = *colptr & mask;
  n1 = 0;
  colptr++;
  if (colptr >= &rowptr[w]) {
    rowptr += stride;  colptr = rowptr;
  }
  colptr2 = colptr;
  for (rowptr2 = rowptr; rowptr2 < dataend;) {
    for (; colptr2 < &rowptr2[w]; colptr2++) {
      ci = (*colptr2) & mask;
      if (ci == c0) {
        n0++;
      } else if (ci == c1) {
        n1++;
      } else
        goto monodone;
    }
    rowptr2 += stride;
    colptr2 = rowptr2;
  }

  monodone:
  if (willTransform) {
    ig->translatePixels(&c0, &c0t, 1);
    ig->translatePixels(&c1, &c1t, 1);
  }
  else {
    c0t = c0;  c1t = c1;
  }

  palette.insert(c0t, n0);
  palette.insert(c1t, n1);

  if (colptr2 >= dataend)
    return;                      // Two colors

  ni = 1;
  colptr2++;
  if (colptr2 >= &rowptr2[w]) {
    rowptr2 += stride;  colptr2 = rowptr2;
  }
  colptr = colptr2;
  for (rowptr = rowptr2; rowptr < dataend;) {
    for (; colptr < &rowptr[w]; colptr++) {
      if (((*colptr) & mask) == ci) {
        ni++;
      } else {
        if (willTransform)
          ig->translatePixels(&ci, &cit, 1);
        else
          cit = ci;
        if (!palette.insert (cit, ni) || (palette.size() > palMaxColors)) {
          palette.clear();
          return;
        }
        ci = (*colptr) & mask;
        ni = 1;
      }
    }
    rowptr += stride;
    colptr = rowptr;
  }
  ig->translatePixels(&ci, &cit, 1);
  if (!palette.insert (cit, ni) || (palette.size() > palMaxColors))
    palette.clear();
}

#endif  // #if (BPP == 8)

bool CHECK_SOLID_TILE(Rect& r, rdr::U32 *colorPtr, bool needSameColor)
{
  const PIXEL_T *buf;
  PIXEL_T colorValue;
  int w = r.width(), h = r.height();

  int stride = w;
  buf = (const PIXEL_T *)ig->getRawBufferR(r, &stride);

  colorValue = *buf;
  if (needSameColor && (rdr::U32)colorValue != *colorPtr)
    return false;

  int bufPad = stride - w;
  while (h > 0) {
    const PIXEL_T *bufEndOfRow = buf + w;
    while (buf < bufEndOfRow) {
      if (colorValue != *(buf++))
        return false;
    }
    buf += bufPad;
    h--;
  }

  *colorPtr = (rdr::U32)colorValue;
  return true;
}

#undef PIXEL_T
#undef TIGHT_ENCODE
#undef HASH_FUNCTION
#undef PACK_PIXELS
#undef ENCODE_SOLID_RECT
#undef ENCODE_FULLCOLOR_RECT
#undef ENCODE_MONO_RECT
#undef ENCODE_INDEXED_RECT
#undef ENCODE_JPEG_RECT
#undef FAST_FILL_PALETTE
#undef FILL_PALETTE
#undef CHECK_SOLID_TILE
}
