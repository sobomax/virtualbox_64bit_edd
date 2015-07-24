/** @file
  Provides a way for 3rd party applications to register themselves for launch by the
  Boot Manager based on hot key

Copyright (c) 2007 - 2012, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#ifndef _HOTKEY_H_
#define _HOTKEY_H_

#include "Bds.h"
#include "String.h"

#define GET_BOOT_OPTION_SUPPORT_KEY_COUNT(a) (((a) & EFI_BOOT_OPTION_SUPPORT_COUNT) >> 8)
#define SET_BOOT_OPTION_SUPPORT_KEY_COUNT(a, c) {  \
      (a) = ((a) & ~EFI_BOOT_OPTION_SUPPORT_COUNT) | (((c) << 8) & EFI_BOOT_OPTION_SUPPORT_COUNT); \
      }

#define BDS_HOTKEY_OPTION_SIGNATURE SIGNATURE_32 ('B', 'd', 'K', 'O')


typedef struct {
  UINTN                     Signature;
  LIST_ENTRY                Link;

  EFI_HANDLE                NotifyHandle;
  UINT16                    BootOptionNumber;
  UINT8                     CodeCount;
  UINT8                     WaitingKey;
  EFI_KEY_DATA              KeyData[3];
} BDS_HOTKEY_OPTION;

#define BDS_HOTKEY_OPTION_FROM_LINK(a) CR (a, BDS_HOTKEY_OPTION, Link, BDS_HOTKEY_OPTION_SIGNATURE)

#define VAR_KEY_ORDER       L"KeyOrder"

/**

  Create Key#### for the given hotkey.


  @param KeyOption       - The Hot Key Option to be added.
  @param KeyOptionNumber - The key option number for Key#### (optional).

  @retval  EFI_SUCCESS            Register hotkey successfully.
  @retval  EFI_INVALID_PARAMETER  The hotkey option is invalid.

**/
EFI_STATUS
RegisterHotkey (
  IN EFI_KEY_OPTION     *KeyOption,
  OUT UINT16            *KeyOptionNumber
  );

/**

  Delete Key#### for the given Key Option number.


  @param KeyOptionNumber - Key option number for Key####

  @retval  EFI_SUCCESS            Unregister hotkey successfully.
  @retval  EFI_NOT_FOUND          No Key#### is found for the given Key Option number.

**/
EFI_STATUS
UnregisterHotkey (
  IN UINT16             KeyOptionNumber
  );


/**

  Process all the "Key####" variables, associate Hotkeys with corresponding Boot Options.


  @param VOID

  @retval  EFI_SUCCESS    Hotkey services successfully initialized.

**/
EFI_STATUS
InitializeHotkeyService (
  VOID
  );

/**
  Try to boot the boot option triggered by hotkey.
  @retval  EFI_SUCCESS             There is HotkeyBootOption & it is processed
  @retval  EFI_NOT_FOUND           There is no HotkeyBootOption
**/
EFI_STATUS
HotkeyBoot (
  VOID
  );

#endif
