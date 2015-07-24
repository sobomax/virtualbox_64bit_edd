/** @file
  This driver produces security architectural protocol based on SecurityManagementLib.
 
  Copyright (c) 2006 - 2009, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials                          
  are licensed and made available under the terms and conditions of the BSD License         
  which accompanies this distribution.  The full text of the license may be found at        
  http://opensource.org/licenses/bsd-license.php                                            

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,                     
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.             

**/


#include <Uefi.h>
#include <Protocol/Security.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/SecurityManagementLib.h>

//
// Handle for the Security Architectural Protocol instance produced by this driver
//
EFI_HANDLE                  mSecurityArchProtocolHandle = NULL;

/**
  The EFI_SECURITY_ARCH_PROTOCOL (SAP) is used to abstract platform-specific 
  policy from the DXE core response to an attempt to use a file that returns a 
  given status for the authentication check from the section extraction protocol.  

  The possible responses in a given SAP implementation may include locking 
  flash upon failure to authenticate, attestation logging for all signed drivers, 
  and other exception operations.  The File parameter allows for possible logging 
  within the SAP of the driver.

  If File is NULL, then EFI_INVALID_PARAMETER is returned.

  If the file specified by File with an authentication status specified by 
  AuthenticationStatus is safe for the DXE Core to use, then EFI_SUCCESS is returned.

  If the file specified by File with an authentication status specified by 
  AuthenticationStatus is not safe for the DXE Core to use under any circumstances, 
  then EFI_ACCESS_DENIED is returned.

  If the file specified by File with an authentication status specified by 
  AuthenticationStatus is not safe for the DXE Core to use right now, but it 
  might be possible to use it at a future time, then EFI_SECURITY_VIOLATION is 
  returned.

  @param  This             The EFI_SECURITY_ARCH_PROTOCOL instance.
  @param  AuthenticationStatus 
                           This is the authentication type returned from the Section
                           Extraction protocol. See the Section Extraction Protocol
                           Specification for details on this type.
  @param  File             This is a pointer to the device path of the file that is
                           being dispatched. This will optionally be used for logging.

  @retval EFI_SUCCESS            Do nothing and return success.
  @retval EFI_INVALID_PARAMETER  File is NULL.
**/
EFI_STATUS
EFIAPI
SecurityStubAuthenticateState (
  IN CONST EFI_SECURITY_ARCH_PROTOCOL  *This,
  IN UINT32                            AuthenticationStatus,
  IN CONST EFI_DEVICE_PATH_PROTOCOL    *File
  )
{
  return ExecuteSecurityHandlers (AuthenticationStatus, File);
}

//
// Security Architectural Protocol instance produced by this driver
//
EFI_SECURITY_ARCH_PROTOCOL  mSecurityStub = { 
  SecurityStubAuthenticateState 
};

/**
  Installs Security Architectural Protocol.

  @param  ImageHandle  The image handle of this driver.
  @param  SystemTable  A pointer to the EFI System Table.
  
  @retval EFI_SUCCESS   Install the sample Security Architectural Protocol successfully.

**/
EFI_STATUS
EFIAPI
SecurityStubInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  //
  // Make sure the Security Architectural Protocol is not already installed in the system
  //
  ASSERT_PROTOCOL_ALREADY_INSTALLED (NULL, &gEfiSecurityArchProtocolGuid);

  //
  // Install the Security Architectural Protocol onto a new handle
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mSecurityArchProtocolHandle,
                  &gEfiSecurityArchProtocolGuid,
                  &mSecurityStub,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
