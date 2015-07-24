/* $Id: ApplianceImpl.h $ */
/** @file
 * VirtualBox COM class implementation
 */

/*
 * Copyright (C) 2006-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ____H_APPLIANCEIMPL
#define ____H_APPLIANCEIMPL

/* VBox includes */
#include "VirtualBoxBase.h"

/* Todo: This file needs massive cleanup. Split IAppliance in a public and
 * private classes. */
#include <iprt/tar.h>
#include <iprt/circbuf.h>
#include <VBox/vd.h>
#include <iprt/sha.h>

#include "ovfreader.h"
#include <set>

/* VBox forward declarations */
class Progress;
class VirtualSystemDescription;
struct VirtualSystemDescriptionEntry;
struct LocationInfo;
typedef struct VDINTERFACE   *PVDINTERFACE;
typedef struct VDINTERFACEIO *PVDINTERFACEIO;
typedef struct SHASTORAGE    *PSHASTORAGE;

typedef enum applianceIOName { applianceIOTar, applianceIOFile, applianceIOSha } APPLIANCEIONAME;

namespace ovf
{
    struct HardDiskController;
    struct VirtualSystem;
    class OVFReader;
    struct DiskImage;
    struct EnvelopeData;
}

namespace xml
{
    class Document;
    class ElementNode;
}

namespace settings
{
    class MachineConfigFile;
}

class ATL_NO_VTABLE Appliance :
    public VirtualBoxBase,
    VBOX_SCRIPTABLE_IMPL(IAppliance)
{
public:
    VIRTUALBOXBASE_ADD_ERRORINFO_SUPPORT(Appliance, IAppliance)

    DECLARE_NOT_AGGREGATABLE(Appliance)

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(Appliance)
        VBOX_DEFAULT_INTERFACE_ENTRIES(IAppliance)
    END_COM_MAP()

    DECLARE_EMPTY_CTOR_DTOR (Appliance)



    // public initializer/uninitializer for internal purposes only
    HRESULT FinalConstruct() { return BaseFinalConstruct(); }
    void FinalRelease() { uninit(); BaseFinalRelease(); }

    HRESULT init(VirtualBox *aVirtualBox);
    void uninit();

    /* IAppliance properties */
    STDMETHOD(COMGETTER(Path))(BSTR *aPath);
    STDMETHOD(COMGETTER(Disks))(ComSafeArrayOut(BSTR, aDisks));
    STDMETHOD(COMGETTER(VirtualSystemDescriptions))(ComSafeArrayOut(IVirtualSystemDescription*, aVirtualSystemDescriptions));
    STDMETHOD(COMGETTER(Machines))(ComSafeArrayOut(BSTR, aMachines));

    /* IAppliance methods */
    /* Import methods */
    STDMETHOD(Read)(IN_BSTR path, IProgress **aProgress);
    STDMETHOD(Interpret)(void);
    STDMETHOD(ImportMachines)(ComSafeArrayIn(ImportOptions_T, options), IProgress **aProgress);
    /* Export methods */
    STDMETHOD(CreateVFSExplorer)(IN_BSTR aURI, IVFSExplorer **aExplorer);
    STDMETHOD(Write)(IN_BSTR format, ComSafeArrayIn(ExportOptions_T, options), IN_BSTR path, IProgress **aProgress);

    STDMETHOD(GetWarnings)(ComSafeArrayOut(BSTR, aWarnings));

    /* public methods only for internal purposes */

    static HRESULT setErrorStatic(HRESULT aResultCode,
                                  const Utf8Str &aText)
    {
        return setErrorInternal(aResultCode, getStaticClassIID(), getStaticComponentName(), aText, false, true);
    }

    /* private instance data */
private:
    /** weak VirtualBox parent */
    VirtualBox* const   mVirtualBox;

    struct ImportStack;
    struct TaskOVF;
    struct Data;            // opaque, defined in ApplianceImpl.cpp
    Data *m;

    enum SetUpProgressMode { ImportFile, ImportS3, WriteFile, WriteS3 };

    /*******************************************************************************
     * General stuff
     ******************************************************************************/

    bool isApplianceIdle();
    HRESULT searchUniqueVMName(Utf8Str& aName) const;
    HRESULT searchUniqueDiskImageFilePath(Utf8Str& aName) const;
    HRESULT setUpProgress(ComObjPtr<Progress> &pProgress,
                          const Bstr &bstrDescription,
                          SetUpProgressMode mode);
    void waitForAsyncProgress(ComObjPtr<Progress> &pProgressThis, ComPtr<IProgress> &pProgressAsync);
    void addWarning(const char* aWarning, ...);
    void disksWeight();
    void parseBucket(Utf8Str &aPath, Utf8Str &aBucket);

    static DECLCALLBACK(int) taskThreadImportOrExport(RTTHREAD aThread, void *pvUser);

    HRESULT initSetOfSupportedStandardsURI();

    Utf8Str typeOfVirtualDiskFormatFromURI(Utf8Str type) const;

    std::set<Utf8Str> URIFromTypeOfVirtualDiskFormat(Utf8Str type);

    HRESULT initApplianceIONameMap();

    Utf8Str applianceIOName(APPLIANCEIONAME type) const;

    /*******************************************************************************
     * Read stuff
     ******************************************************************************/

    HRESULT readImpl(const LocationInfo &aLocInfo, ComObjPtr<Progress> &aProgress);

    HRESULT readFS(TaskOVF *pTask);
    HRESULT readFSOVF(TaskOVF *pTask);
    HRESULT readFSOVA(TaskOVF *pTask);
    HRESULT readFSImpl(TaskOVF *pTask, const RTCString &strFilename, PVDINTERFACEIO pCallbacks, PSHASTORAGE pStorage);
    HRESULT readS3(TaskOVF *pTask);

    /*******************************************************************************
     * Import stuff
     ******************************************************************************/

    HRESULT importImpl(const LocationInfo &aLocInfo, ComObjPtr<Progress> &aProgress);

    HRESULT importFS(TaskOVF *pTask);
    HRESULT importFSOVF(TaskOVF *pTask, AutoWriteLockBase& writeLock);
    HRESULT importFSOVA(TaskOVF *pTask, AutoWriteLockBase& writeLock);
    HRESULT importS3(TaskOVF *pTask);

    HRESULT readFileToBuf(const Utf8Str &strFile,
                             void **ppvBuf,
                             size_t *pcbSize,
                             bool fCreateDigest,
                             PVDINTERFACEIO pCallbacks,
                             PSHASTORAGE pStorage);
    HRESULT readTarFileToBuf(RTTAR tar,
                             const Utf8Str &strFile,
                             void **ppvBuf,
                             size_t *pcbSize,
                             bool fCreateDigest,
                             PVDINTERFACEIO pCallbacks,
                             PSHASTORAGE pStorage);
    HRESULT verifyManifestFile(const Utf8Str &strFile, ImportStack &stack, void *pvBuf, size_t cbSize);

    void convertDiskAttachmentValues(const ovf::HardDiskController &hdc,
                                     uint32_t ulAddressOnParent,
                                     Bstr &controllerType,
                                     int32_t &lControllerPort,
                                     int32_t &lDevice);

    void importOneDiskImage(const ovf::DiskImage &di,
                            Utf8Str *strTargetPath,
                            ComObjPtr<Medium> &pTargetHD,
                            ImportStack &stack,
                            PVDINTERFACEIO pCallbacks,
                            PSHASTORAGE pStorage);

    void importMachineGeneric(const ovf::VirtualSystem &vsysThis,
                              ComObjPtr<VirtualSystemDescription> &vsdescThis,
                              ComPtr<IMachine> &pNewMachine,
                              ImportStack &stack,
                              PVDINTERFACEIO pCallbacks,
                              PSHASTORAGE pStorage);
    void importVBoxMachine(ComObjPtr<VirtualSystemDescription> &vsdescThis,
                           ComPtr<IMachine> &pNewMachine,
                           ImportStack &stack,
                           PVDINTERFACEIO pCallbacks,
                           PSHASTORAGE pStorage);
    void importMachines(ImportStack &stack,
                        PVDINTERFACEIO pCallbacks,
                        PSHASTORAGE pStorage);

    /*******************************************************************************
     * Write stuff
     ******************************************************************************/

    HRESULT writeImpl(ovf::OVFVersion_T aFormat, const LocationInfo &aLocInfo, ComObjPtr<Progress> &aProgress);

    HRESULT writeFS(TaskOVF *pTask);
    HRESULT writeFSOVF(TaskOVF *pTask, AutoWriteLockBase& writeLock);
    HRESULT writeFSOVA(TaskOVF *pTask, AutoWriteLockBase& writeLock);
    HRESULT writeFSImpl(TaskOVF *pTask, AutoWriteLockBase& writeLock, PVDINTERFACEIO pCallbacks, PSHASTORAGE pStorage);
    HRESULT writeS3(TaskOVF *pTask);

    struct XMLStack;

    void buildXML(AutoWriteLockBase& writeLock,
                  xml::Document &doc,
                  XMLStack &stack,
                  const Utf8Str &strPath,
                  ovf::OVFVersion_T enFormat);
    void buildXMLForOneVirtualSystem(AutoWriteLockBase& writeLock,
                                     xml::ElementNode &elmToAddVirtualSystemsTo,
                                     std::list<xml::ElementNode*> *pllElementsWithUuidAttributes,
                                     ComObjPtr<VirtualSystemDescription> &vsdescThis,
                                     ovf::OVFVersion_T enFormat,
                                     XMLStack &stack);

    HRESULT preCheckImageAvailability(PSHASTORAGE pSHAStorage,
                                      RTCString &availableImage);

    friend class Machine;
};

void parseURI(Utf8Str strUri, LocationInfo &locInfo);

struct VirtualSystemDescriptionEntry
{
    uint32_t ulIndex;                       // zero-based index of this entry within array
    VirtualSystemDescriptionType_T type;    // type of this entry
    Utf8Str strRef;                         // reference number (hard disk controllers only)
    Utf8Str strOvf;                         // original OVF value (type-dependent)
    Utf8Str strVBoxSuggested;               // configuration value (type-dependent); original value suggested by interpret()
    Utf8Str strVBoxCurrent;                 // configuration value (type-dependent); current value, either from interpret() or setFinalValue()
    Utf8Str strExtraConfigSuggested;        // extra configuration key=value strings (type-dependent); original value suggested by interpret()
    Utf8Str strExtraConfigCurrent;          // extra configuration key=value strings (type-dependent); current value, either from interpret() or setFinalValue()

    uint32_t ulSizeMB;                      // hard disk images only: a copy of ovf::DiskImage::ulSuggestedSizeMB
    bool skipIt;                            ///< used during export to skip some parts if it's needed
};

class ATL_NO_VTABLE VirtualSystemDescription :
    public VirtualBoxBase,
    VBOX_SCRIPTABLE_IMPL(IVirtualSystemDescription)
{
    friend class Appliance;

public:
    VIRTUALBOXBASE_ADD_ERRORINFO_SUPPORT(VirtualSystemDescription, IVirtualSystemDescription)

    DECLARE_NOT_AGGREGATABLE(VirtualSystemDescription)

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(VirtualSystemDescription)
        VBOX_DEFAULT_INTERFACE_ENTRIES(IVirtualSystemDescription)
    END_COM_MAP()

    DECLARE_EMPTY_CTOR_DTOR (VirtualSystemDescription)

    // public initializer/uninitializer for internal purposes only
    HRESULT FinalConstruct() { return BaseFinalConstruct(); }
    void FinalRelease() { uninit(); BaseFinalRelease(); }

    HRESULT init();
    void uninit();

    /* IVirtualSystemDescription properties */
    STDMETHOD(COMGETTER(Count))(ULONG *aCount);

    /* IVirtualSystemDescription methods */
    STDMETHOD(GetDescription)(ComSafeArrayOut(VirtualSystemDescriptionType_T, aTypes),
                              ComSafeArrayOut(BSTR, aRefs),
                              ComSafeArrayOut(BSTR, aOvfValues),
                              ComSafeArrayOut(BSTR, aVBoxValues),
                              ComSafeArrayOut(BSTR, aExtraConfigValues));

    STDMETHOD(GetDescriptionByType)(VirtualSystemDescriptionType_T aType,
                                    ComSafeArrayOut(VirtualSystemDescriptionType_T, aTypes),
                                    ComSafeArrayOut(BSTR, aRefs),
                                    ComSafeArrayOut(BSTR, aOvfValues),
                                    ComSafeArrayOut(BSTR, aVBoxValues),
                                    ComSafeArrayOut(BSTR, aExtraConfigValues));

    STDMETHOD(GetValuesByType)(VirtualSystemDescriptionType_T aType,
                               VirtualSystemDescriptionValueType_T aWhich,
                               ComSafeArrayOut(BSTR, aValues));

    STDMETHOD(SetFinalValues)(ComSafeArrayIn(BOOL, aEnabled),
                              ComSafeArrayIn(IN_BSTR, aVBoxValues),
                              ComSafeArrayIn(IN_BSTR, aExtraConfigValues));

    STDMETHOD(AddDescription)(VirtualSystemDescriptionType_T aType,
                              IN_BSTR aVBoxValue,
                              IN_BSTR aExtraConfigValue);

    /* public methods only for internal purposes */
    void addEntry(VirtualSystemDescriptionType_T aType,
                  const Utf8Str &strRef,
                  const Utf8Str &aOvfValue,
                  const Utf8Str &aVBoxValue,
                  uint32_t ulSizeMB = 0,
                  const Utf8Str &strExtraConfig = "");

    std::list<VirtualSystemDescriptionEntry*> findByType(VirtualSystemDescriptionType_T aType);
    const VirtualSystemDescriptionEntry* findControllerFromID(uint32_t id);

    void importVBoxMachineXML(const xml::ElementNode &elmMachine);
    const settings::MachineConfigFile* getMachineConfig() const;

    void removeByType(VirtualSystemDescriptionType_T aType);

    /* private instance data */
private:
    struct Data;
    Data *m;

    friend class Machine;
};

#endif // !____H_APPLIANCEIMPL
/* vi: set tabstop=4 shiftwidth=4 expandtab: */
