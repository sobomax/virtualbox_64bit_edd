;;
;; Copyright (C) 2006-2015 Oracle Corporation
;;
;; This file is part of VirtualBox Open Source Edition (OSE), as
;; available from http://www.virtualbox.org. This file is free software;
;; you can redistribute it and/or modify it under the terms of the GNU
;; General Public License (GPL) as published by the Free Software
;; Foundation, in version 2 as it comes in the "COPYING" file of the
;; VirtualBox OSE distribution. VirtualBox OSE is distributed in the
;; hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
;;
;; --------------------------------------------------------------------
;;
;; Shift routines necessary for the OpenWatcom compiler to link code
;; that uses shifts on 64-bit integers
;;

 name    ow_glue64

_TEXT segment word public 'CODE'
      assume  cs:_TEXT

public __U8LS
public __U8RS

__U8LS proc near
        test si, si
        je short out1
loop1:
        sal dx, 1
        rcl cx, 1
        rcl bx, 1
        rcl ax, 1
        dec si
        jne short loop1
out1:
        retn
__U8LS endp

__U8RS proc near
        test si, si
        je short out2
loop2:
        shr ax, 1
        rcr bx, 1
        rcr cx, 1
        rcr dx, 1
        dec si
        jne short loop2
out2:
        retn
__U8RS endp

_TEXT ends

 end
