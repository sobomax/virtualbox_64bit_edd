/* $Id: UIDnDDataObject_win.h $ */
/** @file
 * VBox Qt GUI - UIDnDDataObject class declaration.
 */

/*
 * Copyright (C) 2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___UIDnDDataObject_win_h___
#define ___UIDnDDataObject_win_h___

#include <iprt/critsect.h>

#include <QString>
#include <QStringList>
#include <QVariant>

/* COM includes: */
#include "COMEnums.h"
#include "CDndSource.h"
#include "CSession.h"

/* Forward declarations: */
class UIDnDHandler;

class UIDnDDataObject : public IDataObject
{
public:

    enum Status
    {
        Uninitialized = 0,
        Initialized,
        Dropping,
        Dropped,
        Aborted
    };

public:

    UIDnDDataObject(UIDnDHandler *pDnDHandler, const QStringList &lstFormats);
    virtual ~UIDnDDataObject(void);

public: /* IUnknown methods. */

    STDMETHOD(QueryInterface)(REFIID iid, void ** ppvObject);
    STDMETHOD_(ULONG, AddRef)(void);
    STDMETHOD_(ULONG, Release)(void);

public: /* IDataObject methods. */

    STDMETHOD(GetData)(LPFORMATETC pFormatEtc, LPSTGMEDIUM pMedium);
    STDMETHOD(GetDataHere)(LPFORMATETC pFormatEtc, LPSTGMEDIUM pMedium);
    STDMETHOD(QueryGetData)(LPFORMATETC pFormatEtc);
    STDMETHOD(GetCanonicalFormatEtc)(LPFORMATETC pFormatEct,  LPFORMATETC pFormatEtcOut);
    STDMETHOD(SetData)(LPFORMATETC pFormatEtc, LPSTGMEDIUM pMedium, BOOL fRelease);
    STDMETHOD(EnumFormatEtc)(DWORD dwDirection, IEnumFORMATETC **ppEnumFormatEtc);
    STDMETHOD(DAdvise)(LPFORMATETC pFormatEtc, DWORD advf, IAdviseSink *pAdvSink, DWORD *pdwConnection);
    STDMETHOD(DUnadvise)(DWORD dwConnection);
    STDMETHOD(EnumDAdvise)(IEnumSTATDATA **ppEnumAdvise);

public:

    static const char* ClipboardFormatToString(CLIPFORMAT fmt);

    int Abort(void);
    void SetStatus(Status status);
    int Signal(const QString &strFormat, const void *pvData, uint32_t cbData);

protected:

    bool LookupFormatEtc(LPFORMATETC pFormatEtc, ULONG *puIndex);
    static HGLOBAL MemDup(HGLOBAL hMemSource);
    void RegisterFormat(LPFORMATETC pFormatEtc, CLIPFORMAT clipFormat, TYMED tyMed = TYMED_HGLOBAL,
                        LONG lindex = -1, DWORD dwAspect = DVASPECT_CONTENT, DVTARGETDEVICE *pTargetDevice = NULL);

    UIDnDHandler   *m_pDnDHandler;

    Status          mStatus;
    LONG            mRefCount;
    /** Number of native formats registered. This can be a different number than supplied with mlstFormats. */
    ULONG           mcFormats;
    FORMATETC      *mpFormatEtc;
    STGMEDIUM      *mpStgMedium;
    RTSEMEVENT      mSemEvent;
    QStringList     mlstFormats;
    QString         mstrFormat;
    /** The retrieved data as a QVariant. Needed for buffering in case a second format needs the same data,
     *  e.g. CF_TEXT and CF_UNICODETEXT. */
    QVariant        mVaData;
    /** The retrieved data as a raw buffer. */
    void           *mpvData;
    /** Raw buffer size (in bytes). */
    uint32_t        mcbData;
};

#endif /* ___UIDnDDataObject_win_h___ */

