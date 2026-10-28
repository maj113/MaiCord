#pragma once

// OLE: BSTR, VARIANT, PROPVARIANT, data transfer and OLE initialization, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <ole2.h>
#include <olectl.h>
#include <propsys.h>
#endif

// <wtypes.h>
#define VARIANT_TRUE ((VARIANT_BOOL)-1)
#define VARIANT_FALSE ((VARIANT_BOOL)0)

// <oleauto.h>
#define V_UNION(X, Y) ((X)->Y)
#define V_VT(X) ((X)->vt)
#define V_I4(X) V_UNION(X, lVal)
#define V_R8(X) V_UNION(X, dblVal)
#define V_BSTR(X) V_UNION(X, bstrVal)
#define V_BOOL(X) V_UNION(X, boolVal)
#define V_UNKNOWN(X) V_UNION(X, punkVal)

// <coml2api.h>
#define STGM_READ 0x00000000L

// <olectl.h>
#define CONNECT_E_FIRST MAKE_SCODE(SEVERITY_ERROR, FACILITY_ITF, 0x0200)
#define CONNECT_E_NOCONNECTION (CONNECT_E_FIRST+0)
#define CONNECT_E_ADVISELIMIT (CONNECT_E_FIRST+1)

typedef WORD CLIPFORMAT;
typedef void* HMETAFILEPICT;
typedef double DATE;
typedef OLECHAR* BSTR;
typedef short VARIANT_BOOL;
typedef unsigned short VARTYPE;
typedef LONG DISPID;
typedef WORD PROPVAR_PAD1;
typedef WORD PROPVAR_PAD2;
typedef WORD PROPVAR_PAD3;

MAICORD_SYS_TYPES_BEGIN
struct IAdviseSink;
struct IBindCtx;
struct IDataObject;
struct IDispatch;
struct IEnumFORMATETC;
struct IEnumSTATDATA;
struct IEnumString;
struct IPropertyStore;
struct IRecordInfo;
struct ISequentialStream;
struct IStorage;
struct IStream;
struct tagPROPVARIANT;
struct tagVARIANT;

// <wtypes.h>
enum tagDVASPECT {
    DVASPECT_CONTENT = 1,
};
enum tagSTATFLAG {
    STATFLAG_NONAME = 1,
};
union tagCY {
    struct {
        ULONG Lo;
        LONG Hi;
    };
    LONGLONG int64;
};
typedef tagCY CY;
struct tagDEC {
    USHORT wReserved;
    union {
        struct {
            BYTE scale;
            BYTE sign;
        };
        USHORT signscale;
    };
    ULONG Hi32;
    union {
        struct {
            ULONG Lo32;
            ULONG Mid32;
        };
        ULONGLONG Lo64;
    };
};
typedef tagDEC DECIMAL;
struct tagBSTRBLOB {
    ULONG cbSize;
    BYTE* pData;
};
typedef tagBSTRBLOB BSTRBLOB;
struct tagCLIPDATA {
    ULONG cbSize;
    LONG ulClipFmt;
    BYTE* pClipData;
};
typedef tagCLIPDATA CLIPDATA;
enum VARENUM {
    VT_EMPTY = 0,
    VT_I4 = 3,
    VT_R8 = 5,
    VT_BSTR = 8,
    VT_BOOL = 11,
    VT_UNKNOWN = 13,
    VT_LPWSTR = 31,
};
struct _tagpropertykey {
    GUID fmtid;
    DWORD pid;
};
typedef _tagpropertykey PROPERTYKEY;
// <objidl.h>
struct __declspec(uuid("00000101-0000-0000-C000-000000000046")) __declspec(novtable) IEnumString : IUnknown {
    virtual HRESULT __stdcall Next(ULONG celt, LPOLESTR* rgelt, ULONG* pceltFetched) = 0;
    MAICORD_COM_UNUSED(Skip);
    virtual HRESULT __stdcall Reset() = 0;
    virtual HRESULT __stdcall Clone(IEnumString** ppenum) = 0;
};
struct __declspec(uuid("0c733a30-2a1c-11ce-ade5-00aa0044773d")) __declspec(novtable) ISequentialStream : IUnknown {
    virtual HRESULT __stdcall Read(void* pv, ULONG cb, ULONG* pcbRead) = 0;
    virtual HRESULT __stdcall Write(const void* pv, ULONG cb, ULONG* pcbWritten) = 0;
};
typedef IStream* LPSTREAM;
struct tagSTATSTG {
    LPOLESTR pwcsName;
    DWORD type;
    ULARGE_INTEGER cbSize;
    FILETIME mtime;
    FILETIME ctime;
    FILETIME atime;
    DWORD grfMode;
    DWORD grfLocksSupported;
    CLSID clsid;
    DWORD grfStateBits;
    DWORD reserved;
};
typedef tagSTATSTG STATSTG;
enum tagSTREAM_SEEK {
    STREAM_SEEK_SET = 0,
    STREAM_SEEK_CUR = 1,
};
struct __declspec(uuid("0000000c-0000-0000-C000-000000000046")) __declspec(novtable) IStream : ISequentialStream {
    virtual HRESULT __stdcall Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) = 0;
    MAICORD_COM_UNUSED(SetSize);
    MAICORD_COM_UNUSED(CopyTo);
    virtual HRESULT __stdcall Commit(DWORD grfCommitFlags) = 0;
    MAICORD_COM_UNUSED(Revert);
    MAICORD_COM_UNUSED(LockRegion);
    MAICORD_COM_UNUSED(UnlockRegion);
    virtual HRESULT __stdcall Stat(STATSTG* pstatstg, DWORD grfStatFlag) = 0;
    virtual HRESULT __stdcall Clone(IStream** ppstm) = 0;
};
enum _APTTYPEQUALIFIER {
};
typedef _APTTYPEQUALIFIER APTTYPEQUALIFIER;
enum _APTTYPE {
    APTTYPE_CURRENT = - 1,
    APTTYPE_MTA = 1,
};
typedef _APTTYPE APTTYPE;
struct __declspec(uuid("0000000e-0000-0000-C000-000000000046")) __declspec(novtable) IBindCtx : IUnknown {
};
struct __declspec(uuid("0000000b-0000-0000-C000-000000000046")) __declspec(novtable) IStorage : IUnknown {
    MAICORD_COM_UNUSED(CreateStream);
    MAICORD_COM_UNUSED(OpenStream);
    MAICORD_COM_UNUSED(CreateStorage);
    MAICORD_COM_UNUSED(OpenStorage);
    MAICORD_COM_UNUSED(CopyTo);
    MAICORD_COM_UNUSED(MoveElementTo);
    virtual HRESULT __stdcall Commit(DWORD grfCommitFlags) = 0;
    MAICORD_COM_UNUSED(Revert);
    MAICORD_COM_UNUSED(EnumElements);
    MAICORD_COM_UNUSED(DestroyElement);
    MAICORD_COM_UNUSED(RenameElement);
    MAICORD_COM_UNUSED(SetElementTimes);
    MAICORD_COM_UNUSED(SetClass);
    MAICORD_COM_UNUSED(SetStateBits);
    virtual HRESULT __stdcall Stat(STATSTG* pstatstg, DWORD grfStatFlag) = 0;
};
struct tagDVTARGETDEVICE {
    DWORD tdSize;
    WORD tdDriverNameOffset;
    WORD tdDeviceNameOffset;
    WORD tdPortNameOffset;
    WORD tdExtDevmodeOffset;
    BYTE tdData[1];
};
typedef tagDVTARGETDEVICE DVTARGETDEVICE;
struct tagFORMATETC {
    CLIPFORMAT cfFormat;
    DVTARGETDEVICE* ptd;
    DWORD dwAspect;
    LONG lindex;
    DWORD tymed;
};
typedef tagFORMATETC FORMATETC;
struct __declspec(uuid("00000103-0000-0000-C000-000000000046")) __declspec(novtable) IEnumFORMATETC : IUnknown {
    virtual HRESULT __stdcall Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) = 0;
    MAICORD_COM_UNUSED(Skip);
    virtual HRESULT __stdcall Reset() = 0;
    virtual HRESULT __stdcall Clone(IEnumFORMATETC** ppenum) = 0;
};
struct tagSTATDATA {
    FORMATETC formatetc;
    DWORD advf;
    IAdviseSink* pAdvSink;
    DWORD dwConnection;
};
typedef tagSTATDATA STATDATA;
struct __declspec(uuid("00000105-0000-0000-C000-000000000046")) __declspec(novtable) IEnumSTATDATA : IUnknown {
    virtual HRESULT __stdcall Next(ULONG celt, STATDATA* rgelt, ULONG* pceltFetched) = 0;
    MAICORD_COM_UNUSED(Skip);
    virtual HRESULT __stdcall Reset() = 0;
    virtual HRESULT __stdcall Clone(IEnumSTATDATA** ppenum) = 0;
};
enum tagTYMED {
    TYMED_HGLOBAL = 1,
    TYMED_ISTREAM = 4,
};
struct tagSTGMEDIUM {
    DWORD tymed;
    union {
        HBITMAP hBitmap;
        HMETAFILEPICT hMetaFilePict;
        HENHMETAFILE hEnhMetaFile;
        HGLOBAL hGlobal;
        LPOLESTR lpszFileName;
        IStream* pstm;
        IStorage* pstg;
    };
    IUnknown* pUnkForRelease;
};
typedef tagSTGMEDIUM uSTGMEDIUM;
typedef uSTGMEDIUM STGMEDIUM;
typedef STGMEDIUM* LPSTGMEDIUM;
struct __declspec(uuid("0000010f-0000-0000-C000-000000000046")) __declspec(novtable) IAdviseSink : IUnknown {
};
typedef IDataObject* LPDATAOBJECT;
enum tagDATADIR {
    DATADIR_GET = 1,
};
struct __declspec(uuid("0000010e-0000-0000-C000-000000000046")) __declspec(novtable) IDataObject : IUnknown {
    virtual HRESULT __stdcall GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) = 0;
    virtual HRESULT __stdcall GetDataHere(FORMATETC* pformatetc, STGMEDIUM* pmedium) = 0;
    virtual HRESULT __stdcall QueryGetData(FORMATETC* pformatetc) = 0;
    virtual HRESULT __stdcall GetCanonicalFormatEtc(FORMATETC* pformatectIn, FORMATETC* pformatetcOut) = 0;
    virtual HRESULT __stdcall SetData(FORMATETC* pformatetc, STGMEDIUM* pmedium, BOOL fRelease) = 0;
    virtual HRESULT __stdcall EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) = 0;
    virtual HRESULT __stdcall DAdvise(FORMATETC* pformatetc, DWORD advf, IAdviseSink* pAdvSink,
        DWORD* pdwConnection) = 0;
    virtual HRESULT __stdcall DUnadvise(DWORD dwConnection) = 0;
    virtual HRESULT __stdcall EnumDAdvise(IEnumSTATDATA** ppenumAdvise) = 0;
};
// <oaidl.h>
struct tagSAFEARRAYBOUND {
    ULONG cElements;
    LONG lLbound;
};
typedef tagSAFEARRAYBOUND SAFEARRAYBOUND;
struct tagSAFEARRAY {
    USHORT cDims;
    USHORT fFeatures;
    ULONG cbElements;
    ULONG cLocks;
    PVOID pvData;
    SAFEARRAYBOUND rgsabound[1];
};
typedef tagSAFEARRAY SAFEARRAY;
typedef SAFEARRAY* LPSAFEARRAY;
typedef struct tagVARIANT VARIANT;
struct tagVARIANT {
    union {
        struct {
            VARTYPE vt;
            WORD wReserved1;
            WORD wReserved2;
            WORD wReserved3;
            union {
                LONGLONG llVal;
                LONG lVal;
                BYTE bVal;
                SHORT iVal;
                FLOAT fltVal;
                DOUBLE dblVal;
                VARIANT_BOOL boolVal;
                VARIANT_BOOL __OBSOLETE__VARIANT_BOOL;
                SCODE scode;
                CY cyVal;
                DATE date;
                BSTR bstrVal;
                IUnknown* punkVal;
                IDispatch* pdispVal;
                SAFEARRAY* parray;
                BYTE* pbVal;
                SHORT* piVal;
                LONG* plVal;
                LONGLONG* pllVal;
                FLOAT* pfltVal;
                DOUBLE* pdblVal;
                VARIANT_BOOL* pboolVal;
                VARIANT_BOOL* __OBSOLETE__VARIANT_PBOOL;
                SCODE* pscode;
                CY* pcyVal;
                DATE* pdate;
                BSTR* pbstrVal;
                IUnknown** ppunkVal;
                IDispatch** ppdispVal;
                SAFEARRAY** pparray;
                VARIANT* pvarVal;
                PVOID byref;
                CHAR cVal;
                USHORT uiVal;
                ULONG ulVal;
                ULONGLONG ullVal;
                INT intVal;
                UINT uintVal;
                DECIMAL* pdecVal;
                CHAR* pcVal;
                USHORT* puiVal;
                ULONG* pulVal;
                ULONGLONG* pullVal;
                INT* pintVal;
                UINT* puintVal;
                struct {
                    PVOID pvRecord;
                    IRecordInfo* pRecInfo;
                };
            };
        };
        DECIMAL decVal;
    };
};
typedef VARIANT VARIANTARG;
struct tagDISPPARAMS {
    VARIANTARG* rgvarg;
    DISPID* rgdispidNamedArgs;
    UINT cArgs;
    UINT cNamedArgs;
};
typedef tagDISPPARAMS DISPPARAMS;
struct tagEXCEPINFO {
    WORD wCode;
    WORD wReserved;
    BSTR bstrSource;
    BSTR bstrDescription;
    BSTR bstrHelpFile;
    DWORD dwHelpContext;
    PVOID pvReserved;
    HRESULT(__stdcall* pfnDeferredFillIn)(struct tagEXCEPINFO*);
    SCODE scode;
};
typedef tagEXCEPINFO EXCEPINFO;
struct __declspec(uuid("00020400-0000-0000-C000-000000000046")) __declspec(novtable) IDispatch : IUnknown {
    MAICORD_COM_UNUSED(GetTypeInfoCount);
    MAICORD_COM_UNUSED(GetTypeInfo);
    MAICORD_COM_UNUSED(GetIDsOfNames);
    virtual HRESULT __stdcall Invoke(DISPID dispIdMember, const IID& riid, LCID lcid, WORD wFlags,
        DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr) = 0;
};
struct __declspec(uuid("0000002F-0000-0000-C000-000000000046")) __declspec(novtable) IRecordInfo : IUnknown {
};
// <propidlbase.h>
struct tagVersionedStream {
    GUID guidVersion;
    IStream* pStream;
};
typedef struct tagVersionedStream* LPVERSIONEDSTREAM;
typedef struct tagPROPVARIANT PROPVARIANT;
struct tagCAC {
    ULONG cElems;
    CHAR* pElems;
};
typedef tagCAC CAC;
struct tagCAUB {
    ULONG cElems;
    UCHAR* pElems;
};
typedef tagCAUB CAUB;
struct tagCAI {
    ULONG cElems;
    SHORT* pElems;
};
typedef tagCAI CAI;
struct tagCAUI {
    ULONG cElems;
    USHORT* pElems;
};
typedef tagCAUI CAUI;
struct tagCAL {
    ULONG cElems;
    LONG* pElems;
};
typedef tagCAL CAL;
struct tagCAUL {
    ULONG cElems;
    ULONG* pElems;
};
typedef tagCAUL CAUL;
struct tagCAFLT {
    ULONG cElems;
    FLOAT* pElems;
};
typedef tagCAFLT CAFLT;
struct tagCADBL {
    ULONG cElems;
    DOUBLE* pElems;
};
typedef tagCADBL CADBL;
struct tagCACY {
    ULONG cElems;
    CY* pElems;
};
typedef tagCACY CACY;
struct tagCADATE {
    ULONG cElems;
    DATE* pElems;
};
typedef tagCADATE CADATE;
struct tagCABSTR {
    ULONG cElems;
    BSTR* pElems;
};
typedef tagCABSTR CABSTR;
struct tagCABSTRBLOB {
    ULONG cElems;
    BSTRBLOB* pElems;
};
typedef tagCABSTRBLOB CABSTRBLOB;
struct tagCABOOL {
    ULONG cElems;
    VARIANT_BOOL* pElems;
};
typedef tagCABOOL CABOOL;
struct tagCASCODE {
    ULONG cElems;
    SCODE* pElems;
};
typedef tagCASCODE CASCODE;
struct tagCAPROPVARIANT {
    ULONG cElems;
    PROPVARIANT* pElems;
};
typedef tagCAPROPVARIANT CAPROPVARIANT;
struct tagCAH {
    ULONG cElems;
    LARGE_INTEGER* pElems;
};
typedef tagCAH CAH;
struct tagCAUH {
    ULONG cElems;
    ULARGE_INTEGER* pElems;
};
typedef tagCAUH CAUH;
struct tagCALPSTR {
    ULONG cElems;
    LPSTR* pElems;
};
typedef tagCALPSTR CALPSTR;
struct tagCALPWSTR {
    ULONG cElems;
    LPWSTR* pElems;
};
typedef tagCALPWSTR CALPWSTR;
struct tagCAFILETIME {
    ULONG cElems;
    FILETIME* pElems;
};
typedef tagCAFILETIME CAFILETIME;
struct tagCACLIPDATA {
    ULONG cElems;
    CLIPDATA* pElems;
};
typedef tagCACLIPDATA CACLIPDATA;
struct tagCACLSID {
    ULONG cElems;
    CLSID* pElems;
};
typedef tagCACLSID CACLSID;
struct tagPROPVARIANT {
    union {
        struct {
            VARTYPE vt;
            PROPVAR_PAD1 wReserved1;
            PROPVAR_PAD2 wReserved2;
            PROPVAR_PAD3 wReserved3;
            union {
                CHAR cVal;
                UCHAR bVal;
                SHORT iVal;
                USHORT uiVal;
                LONG lVal;
                ULONG ulVal;
                INT intVal;
                UINT uintVal;
                LARGE_INTEGER hVal;
                ULARGE_INTEGER uhVal;
                FLOAT fltVal;
                DOUBLE dblVal;
                VARIANT_BOOL boolVal;
                VARIANT_BOOL __OBSOLETE__VARIANT_BOOL;
                SCODE scode;
                CY cyVal;
                DATE date;
                FILETIME filetime;
                CLSID* puuid;
                CLIPDATA* pclipdata;
                BSTR bstrVal;
                BSTRBLOB bstrblobVal;
                BLOB blob;
                LPSTR pszVal;
                LPWSTR pwszVal;
                IUnknown* punkVal;
                IDispatch* pdispVal;
                IStream* pStream;
                IStorage* pStorage;
                LPVERSIONEDSTREAM pVersionedStream;
                LPSAFEARRAY parray;
                CAC cac;
                CAUB caub;
                CAI cai;
                CAUI caui;
                CAL cal;
                CAUL caul;
                CAH cah;
                CAUH cauh;
                CAFLT caflt;
                CADBL cadbl;
                CABOOL cabool;
                CASCODE cascode;
                CACY cacy;
                CADATE cadate;
                CAFILETIME cafiletime;
                CACLSID cauuid;
                CACLIPDATA caclipdata;
                CABSTR cabstr;
                CABSTRBLOB cabstrblob;
                CALPSTR calpstr;
                CALPWSTR calpwstr;
                CAPROPVARIANT capropvar;
                CHAR* pcVal;
                UCHAR* pbVal;
                SHORT* piVal;
                USHORT* puiVal;
                LONG* plVal;
                ULONG* pulVal;
                INT* pintVal;
                UINT* puintVal;
                FLOAT* pfltVal;
                DOUBLE* pdblVal;
                VARIANT_BOOL* pboolVal;
                DECIMAL* pdecVal;
                SCODE* pscode;
                CY* pcyVal;
                DATE* pdate;
                BSTR* pbstrVal;
                IUnknown** ppunkVal;
                IDispatch** ppdispVal;
                LPSAFEARRAY* pparray;
                PROPVARIANT* pvarVal;
            };
        };
        DECIMAL decVal;
    };
};
// <propsys.h>
struct __declspec(uuid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99")) __declspec(novtable) IPropertyStore : IUnknown {
    virtual HRESULT __stdcall GetCount(DWORD* cProps) = 0;
    MAICORD_COM_UNUSED(GetAt);
    virtual HRESULT __stdcall GetValue(const PROPERTYKEY& key, PROPVARIANT* pv) = 0;
    virtual HRESULT __stdcall SetValue(const PROPERTYKEY& key, const PROPVARIANT& propvar) = 0;
    virtual HRESULT __stdcall Commit() = 0;
};
MAICORD_SYS_TYPES_END

// <oleauto.h>
extern "C" __declspec(dllimport) BSTR WINAPI SysAllocString(const OLECHAR* psz);
extern "C" __declspec(dllimport) void WINAPI SysFreeString(BSTR bstrString);
extern "C" __declspec(dllimport) HRESULT WINAPI SafeArrayDestroy(SAFEARRAY* psa);
extern "C" __declspec(dllimport) HRESULT WINAPI SafeArrayGetUBound(SAFEARRAY* psa, UINT nDim, LONG* plUbound);
extern "C" __declspec(dllimport) HRESULT WINAPI SafeArrayGetElement(SAFEARRAY* psa, LONG* rgIndices, void* pv);
extern "C" __declspec(dllimport) HRESULT WINAPI VariantClear(VARIANTARG* pvarg);

// <ole2.h>
extern "C" __declspec(dllimport) HRESULT WINAPI OleInitialize(LPVOID pvReserved);
extern "C" __declspec(dllimport) void WINAPI OleUninitialize();
extern "C" __declspec(dllimport) HRESULT WINAPI OleGetClipboard(LPDATAOBJECT* ppDataObj);
extern "C" __declspec(dllimport) void WINAPI ReleaseStgMedium(LPSTGMEDIUM);

// <combaseapi.h>
extern "C" __declspec(dllimport) HRESULT WINAPI CreateStreamOnHGlobal(HGLOBAL hGlobal, BOOL fDeleteOnRelease,
    LPSTREAM* ppstm);
extern "C" __declspec(dllimport) HRESULT WINAPI CoGetApartmentType(APTTYPE* pAptType, APTTYPEQUALIFIER* pAptQualifier);
extern "C" __declspec(dllimport) HRESULT WINAPI CoReleaseMarshalData(LPSTREAM pStm);
extern "C" __declspec(dllimport) HRESULT WINAPI CoMarshalInterThreadInterfaceInStream(const IID& riid, LPUNKNOWN pUnk,
    LPSTREAM* ppStm);
extern "C" __declspec(dllimport) HRESULT WINAPI CoGetInterfaceAndReleaseStream(LPSTREAM pStm, const IID& iid,
    LPVOID* ppv);
extern "C" __declspec(dllimport) HRESULT WINAPI PropVariantClear(PROPVARIANT* pvar);
