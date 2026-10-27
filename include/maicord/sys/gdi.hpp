#pragma once

// gdi32, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_types.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

// <wingdi.h>
#define GDI_ERROR (0xFFFFFFFFL)
#define HGDI_ERROR (LongToHandle(0xFFFFFFFFL))
#define LCS_sRGB 'sRGB'
#define LCS_GM_IMAGES 0x00000004L
#define BI_RGB 0L
#define BI_BITFIELDS 3L
#define LF_FACESIZE 32
#define OUT_OUTLINE_PRECIS 8
#define DEFAULT_CHARSET 1
#define GetRValue(rgb) (LOBYTE(rgb))
#define GetGValue(rgb) (LOBYTE(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) (LOBYTE((rgb)>>16))
#define GetGlyphIndices GetGlyphIndicesW

typedef long FXPT2DOT30;

MAICORD_SYS_TYPES_BEGIN
// <wingdi.h>
struct tagCIEXYZ {
    FXPT2DOT30 ciexyzX;
    FXPT2DOT30 ciexyzY;
    FXPT2DOT30 ciexyzZ;
};
typedef tagCIEXYZ CIEXYZ;
struct tagICEXYZTRIPLE {
    CIEXYZ ciexyzRed;
    CIEXYZ ciexyzGreen;
    CIEXYZ ciexyzBlue;
};
typedef tagICEXYZTRIPLE CIEXYZTRIPLE;
struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
};
typedef tagBITMAPINFOHEADER BITMAPINFOHEADER;
typedef struct {
    DWORD bV5Size;
    LONG bV5Width;
    LONG bV5Height;
    WORD bV5Planes;
    WORD bV5BitCount;
    DWORD bV5Compression;
    DWORD bV5SizeImage;
    LONG bV5XPelsPerMeter;
    LONG bV5YPelsPerMeter;
    DWORD bV5ClrUsed;
    DWORD bV5ClrImportant;
    DWORD bV5RedMask;
    DWORD bV5GreenMask;
    DWORD bV5BlueMask;
    DWORD bV5AlphaMask;
    DWORD bV5CSType;
    CIEXYZTRIPLE bV5Endpoints;
    DWORD bV5GammaRed;
    DWORD bV5GammaGreen;
    DWORD bV5GammaBlue;
    DWORD bV5Intent;
    DWORD bV5ProfileData;
    DWORD bV5ProfileSize;
    DWORD bV5Reserved;
} BITMAPV5HEADER;
#pragma pack(push, 2)
struct tagBITMAPFILEHEADER {
    WORD bfType;
    DWORD bfSize;
    WORD bfReserved1;
    WORD bfReserved2;
    DWORD bfOffBits;
};
#pragma pack(pop)
typedef tagBITMAPFILEHEADER BITMAPFILEHEADER;
struct tagLOGFONTW {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    WCHAR lfFaceName[32];
};
typedef tagLOGFONTW LOGFONTW;
MAICORD_SYS_TYPES_END

// <wingdi.h>
extern "C" __declspec(dllimport) HDC WINAPI CreateCompatibleDC(HDC hdc);
extern "C" __declspec(dllimport) HFONT WINAPI CreateFontIndirectW(const LOGFONTW* lplf);
extern "C" __declspec(dllimport) BOOL WINAPI DeleteDC(HDC hdc);
extern "C" __declspec(dllimport) BOOL WINAPI DeleteObject(HGDIOBJ ho);
extern "C" __declspec(dllimport) DWORD WINAPI GetFontData(HDC hdc, DWORD dwTable, DWORD dwOffset, PVOID pvBuffer,
    DWORD cjBuffer);
extern "C" __declspec(dllimport) DWORD WINAPI GetGlyphIndicesW(HDC hdc, LPCWSTR lpstr, int c, LPWORD pgi, DWORD fl);
extern "C" __declspec(dllimport) HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h);
extern "C" __declspec(dllimport) COLORREF WINAPI SetTextColor(HDC hdc, COLORREF color);
extern "C" __declspec(dllimport) int WINAPI GetTextFaceW(HDC hdc, int c, LPWSTR lpName);
