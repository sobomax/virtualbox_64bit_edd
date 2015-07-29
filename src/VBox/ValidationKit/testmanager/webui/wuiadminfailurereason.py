# -*- coding: utf-8 -*-
# $Id: wuiadminfailurereason.py $

"""
Test Manager WUI - Failure Reasons Web content generator.
"""

__copyright__ = \
"""
Copyright (C) 2012-2015 Oracle Corporation

This file is part of VirtualBox Open Source Edition (OSE), as
available from http://www.virtualbox.org. This file is free software;
you can redistribute it and/or modify it under the terms of the GNU
General Public License (GPL) as published by the Free Software
Foundation, in version 2 as it comes in the "COPYING" file of the
VirtualBox OSE distribution. VirtualBox OSE is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.

The contents of this file may alternatively be used under the terms
of the Common Development and Distribution License Version 1.0
(CDDL) only, as it comes in the "COPYING.CDDL" file of the
VirtualBox OSE distribution, in which case the provisions of the
CDDL are applicable instead of those of the GPL.

You may elect to license modified versions of this file under the
terms and conditions of either the GPL or the CDDL or both.
"""
__version__ = "$Revision: 100880 $"


# Validation Kit imports.
from testmanager.webui.wuibase        import WuiException
from testmanager.webui.wuicontentbase import WuiFormContentBase, WuiListContentBase, WuiTmLink
from testmanager.core.failurereason   import FailureReasonData
from testmanager.core.failurecategory import FailureCategoryLogic
from testmanager.core.db              import TMDatabaseConnection


class WuiAdminFailureReason(WuiFormContentBase):
    """
    WUI Failure Reason HTML content generator.
    """

    def __init__(self, oFailureReasonData, sMode, oDisp):
        """
        Prepare & initialize parent
        """

        if sMode == WuiFormContentBase.ksMode_Add:
            sTitle = 'Add Failure Reason'
            sSubmitAction = oDisp.ksActionFailureReasonAdd
        elif sMode == WuiFormContentBase.ksMode_Edit:
            sTitle = 'Edit Failure Reason'
            sSubmitAction = oDisp.ksActionFailureReasonEdit
        else:
            raise WuiException('Unknown parameter')

        WuiFormContentBase.__init__(self, oFailureReasonData, sMode, 'FailureReason', oDisp, sTitle,
                                    sSubmitAction = sSubmitAction, fEditable = False); ## @todo non-standard action names.

    def _populateForm(self, oForm, oData):
        """
        Construct an HTML form
        """

        aoFailureCategories = FailureCategoryLogic(TMDatabaseConnection()).getFailureCategoriesForCombo()
        if len(aoFailureCategories) == 0:
            from testmanager.webui.wuiadmin import WuiAdmin
            sExceptionMsg = 'Please <a href="%s?%s=%s">add</a> Failure Category first.' % \
                (WuiAdmin.ksScriptName, WuiAdmin.ksParamAction, WuiAdmin.ksActionFailureCategoryShowAdd)

            raise WuiException(sExceptionMsg)

        oForm.addIntRO        (FailureReasonData.ksParam_idFailureReason,    oData.idFailureReason,    'Failure Reason ID')
        oForm.addTimestampRO  (FailureReasonData.ksParam_tsEffective,        oData.tsEffective,        'Last changed')
        oForm.addTimestampRO  (FailureReasonData.ksParam_tsExpire,           oData.tsExpire,           'Expires (excl)')
        oForm.addIntRO        (FailureReasonData.ksParam_uidAuthor,          oData.uidAuthor,          'Changed by UID')

        oForm.addComboBox     (FailureReasonData.ksParam_idFailureCategory,  oData.idFailureCategory,  'Failure Category',
                               aoFailureCategories)

        oForm.addText         (FailureReasonData.ksParam_sShort,             oData.sShort,             'Short Description')
        oForm.addText         (FailureReasonData.ksParam_sFull,              oData.sFull,              'Full Description')
        oForm.addInt          (FailureReasonData.ksParam_iTicket,            oData.iTicket,            'Ticket Number')
        oForm.addMultilineText(FailureReasonData.ksParam_asUrls,             oData.asUrls,             'Other URLs to reports '
                                                                                                       'or discussions of the '
                                                                                                       'observed symptoms')
        oForm.addSubmit()

        return True

class WuiAdminFailureReasonList(WuiListContentBase):
    """
    WUI Admin Failure Reasons Content Generator.
    """

    def __init__(self, aoEntries, iPage, cItemsPerPage, tsEffective, fnDPrint, oDisp):
        WuiListContentBase.__init__(self, aoEntries, iPage, cItemsPerPage, tsEffective,
                                    sTitle = 'Failure Reasons', sId = 'failureReasons',
                                    fnDPrint = fnDPrint, oDisp = oDisp);

        self._asColumnHeaders = ['ID', 'Category', 'Short Description',
                                 'Full Description', 'Ticket', 'External References', 'Actions' ]

        self._asColumnAttribs = ['align="right"', 'align="center"', 'align="center"',
                                 'align="center"',' align="center"', 'align="center"', 'align="center"']

    def _formatListEntry(self, iEntry):
        from testmanager.webui.wuiadmin import WuiAdmin
        oEntry = self._aoEntries[iEntry]

        return [ oEntry.idFailureReason,
                 oEntry.idFailureCategory,
                 oEntry.sShort,
                 oEntry.sFull,
                 oEntry.iTicket,
                 oEntry.asUrls,
                 [ WuiTmLink('Modify', WuiAdmin.ksScriptName,
                             { WuiAdmin.ksParamAction: WuiAdmin.ksActionFailureReasonShowEdit,
                               FailureReasonData.ksParam_idFailureReason: oEntry.idFailureReason } ),
                   WuiTmLink('Remove', WuiAdmin.ksScriptName,
                             { WuiAdmin.ksParamAction: WuiAdmin.ksActionFailureReasonDel,
                               FailureReasonData.ksParam_idFailureReason: oEntry.idFailureReason },
                             sConfirm = 'Are you sure you want to remove failure reason #%d?' % (oEntry.idFailureReason,)),
               ] ]
