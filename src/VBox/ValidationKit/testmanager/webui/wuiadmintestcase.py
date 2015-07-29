# -*- coding: utf-8 -*-
# $Id: wuiadmintestcase.py $

"""
Test Manager WUI - Test Cases.
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
from common                             import utils, webutils;
from testmanager.webui.wuicontentbase   import WuiFormContentBase, WuiListContentBase, WuiTmLink, WuiRawHtml;
from testmanager.core.db                import isDbTimestampInfinity;
from testmanager.core.testcase          import TestCaseDataEx, TestCaseData, TestCaseDependencyLogic;
from testmanager.core.globalresource    import GlobalResourceData, GlobalResourceLogic;

class WuiTestCaseList(WuiListContentBase):
    """
    WUI test case list content generator.
    """

    def __init__(self, aoEntries, iPage, cItemsPerPage, tsEffective, fnDPrint, oDisp):
        WuiListContentBase.__init__(self, aoEntries, iPage, cItemsPerPage, tsEffective,
                                    sTitle = 'Test Cases', fnDPrint = fnDPrint, oDisp = oDisp);
        self._asColumnHeaders = \
        [
            'Name', 'Active', 'Default Timeout', 'Base Command / Variations', 'Validation Kit Files',
            'Test Case Prereqs', 'Global Resources', 'Actions'
        ];
        self._asColumnAttribs = \
        [
            '', '', '', '', '',
            'valign="top"', 'valign="top"', 'align="center"'
        ];

    def _formatListEntry(self, iEntry):
        oEntry = self._aoEntries[iEntry];
        from testmanager.webui.wuiadmin import WuiAdmin;

        aoRet = \
        [
            oEntry.sName.replace('-', u'\u2011'),
            'Enabled' if oEntry.fEnabled else 'Disabled',
            utils.formatIntervalSeconds(oEntry.cSecTimeout),
        ];

        # Base command and variations.
        fNoGang = True;
        for oVar in oEntry.aoTestCaseArgs:
            if oVar.cGangMembers > 1:
                fNoGang = False;
                break;
        sHtml  = '  <table class="tminnertbl" width=100%>\n' \
                 '    <tr>\n' \
                 '      <th>';
        if not fNoGang:
            sHtml += '<th>Gang Size</th>';
        sHtml += 'Timeout</th><th>Additional Arguments</b></th>\n' \
                 '    </tr>\n'
        for oTmp in oEntry.aoTestCaseArgs:
            sHtml += '<tr>';
            if not fNoGang:
                sHtml += '<td>%d</td>' % (oTmp.cGangMembers,)
            sHtml += '<td>%s</td><td>%s</td></tr>\n' \
                   % ( utils.formatIntervalSeconds(oTmp.cSecTimeout) if oTmp.cSecTimeout is not None else 'Default',
                       webutils.escapeElem(oTmp.sArgs.replace('-', u'\u2011')),)
        sHtml += '  </table>'

        aoRet.append([oEntry.sBaseCmd.replace('-', u'\u2011'), WuiRawHtml(sHtml)]);

        # Next.
        aoRet += [ oEntry.sValidationKitZips if oEntry.sValidationKitZips is not None else '', ];

        # Show dependency on other testcases
        if oEntry.aoDepTestCases not in (None, []):
            sHtml = '  <ul class="tmshowall">\n'
            for sTmp in oEntry.aoDepTestCases:
                sHtml += '    <li class="tmshowall"><a href="%s?%s=%s&%s=%s">%s</a></li>\n' \
                       % (WuiAdmin.ksScriptName,
                          WuiAdmin.ksParamAction, WuiAdmin.ksActionTestCaseEdit,
                          TestCaseData.ksParam_idTestCase, sTmp.idTestCase,
                          sTmp.sName)
            sHtml += '  </ul>\n'
        else:
            sHtml = '<ul class="tmshowall"><li class="tmshowall">None</li></ul>\n'
        aoRet.append(WuiRawHtml(sHtml));

        # Show dependency on global resources
        if oEntry.aoDepGlobalResources not in (None, []):
            sHtml = '  <ul class="tmshowall">\n'
            for sTmp in oEntry.aoDepGlobalResources:
                sHtml += '    <li class="tmshowall"><a href="%s?%s=%s&%s=%s">%s</a></li>\n' \
                       % (WuiAdmin.ksScriptName,
                          WuiAdmin.ksParamAction, WuiAdmin.ksActionGlobalRsrcShowEdit,
                          GlobalResourceData.ksParam_idGlobalRsrc, sTmp.idGlobalRsrc,
                          sTmp.sName)
            sHtml += '  </ul>\n'
        else:
            sHtml = '<ul class="tmshowall"><li class="tmshowall">None</li></ul>\n'
        aoRet.append(WuiRawHtml(sHtml));

        # Show actions that can be taken.
        aoActions = [ WuiTmLink('Details', WuiAdmin.ksScriptName,
                                { WuiAdmin.ksParamAction: WuiAdmin.ksActionTestCaseDetails,
                                  TestCaseData.ksParam_idGenTestCase: oEntry.idGenTestCase }), ];
        if isDbTimestampInfinity(oEntry.tsExpire):
            aoActions.append(WuiTmLink('Modify', WuiAdmin.ksScriptName,
                                       { WuiAdmin.ksParamAction: WuiAdmin.ksActionTestCaseEdit,
                                         TestCaseData.ksParam_idTestCase: oEntry.idTestCase }));
        aoActions.append(WuiTmLink('Clone', WuiAdmin.ksScriptName,
                                   { WuiAdmin.ksParamAction: WuiAdmin.ksActionTestCaseClone,
                                     TestCaseData.ksParam_idGenTestCase: oEntry.idGenTestCase }));
        if isDbTimestampInfinity(oEntry.tsExpire):
            aoActions.append(WuiTmLink('Remove', WuiAdmin.ksScriptName,
                                       { WuiAdmin.ksParamAction: WuiAdmin.ksActionTestCaseDoRemove,
                                         TestCaseData.ksParam_idTestCase: oEntry.idTestCase },
                                       sConfirm = 'Are you sure you want to remove test case #%d?' % (oEntry.idTestCase,)));
        aoRet.append(aoActions);

        return aoRet;


class WuiTestCase(WuiFormContentBase):
    """
    WUI user account content generator.
    """

    def __init__(self, oData, sMode, oDisp):
        assert isinstance(oData, TestCaseDataEx);

        if sMode == WuiFormContentBase.ksMode_Add:
            sTitle = 'New Test Case';
        elif sMode == WuiFormContentBase.ksMode_Edit:
            sTitle = 'Edit Test Case - %s (#%s)' % (oData.sName, oData.idTestCase);
        else:
            assert sMode == WuiFormContentBase.ksMode_Show;
            sTitle = 'Test Case - %s (#%s)' % (oData.sName, oData.idTestCase);
        WuiFormContentBase.__init__(self, oData, sMode, 'TestCase', oDisp, sTitle);

        # Read additional bits form the DB.
        oDepLogic = TestCaseDependencyLogic(oDisp.getDb());
        self._aoAllTestCases    = oDepLogic.getApplicableDepTestCaseData(-1 if oData.idTestCase is None else oData.idTestCase);
        self._aoAllGlobalRsrcs  = GlobalResourceLogic(oDisp.getDb()).getAll();

    def _populateForm(self, oForm, oData):
        oForm.addIntRO      (TestCaseData.ksParam_idTestCase,       oData.idTestCase,       'Test Case ID')
        oForm.addTimestampRO(TestCaseData.ksParam_tsEffective,      oData.tsEffective,      'Last changed')
        oForm.addTimestampRO(TestCaseData.ksParam_tsExpire,         oData.tsExpire,         'Expires (excl)')
        oForm.addIntRO      (TestCaseData.ksParam_uidAuthor,        oData.uidAuthor,        'Changed by UID')
        oForm.addIntRO      (TestCaseData.ksParam_idGenTestCase,    oData.idGenTestCase,    'Test Case generation ID')
        oForm.addText       (TestCaseData.ksParam_sName,            oData.sName,            'Name')
        oForm.addText       (TestCaseData.ksParam_sDescription,     oData.sDescription,     'Description')
        oForm.addCheckBox   (TestCaseData.ksParam_fEnabled,         oData.fEnabled,         'Enabled')
        oForm.addLong       (TestCaseData.ksParam_cSecTimeout,
                             utils.formatIntervalSeconds2(oData.cSecTimeout),               'Default timeout')
        oForm.addWideText   (TestCaseData.ksParam_sTestBoxReqExpr,  oData.sTestBoxReqExpr,  'TestBox requirements (python)');
        oForm.addWideText   (TestCaseData.ksParam_sBuildReqExpr,    oData.sBuildReqExpr,    'Build requirement (python)');
        oForm.addWideText   (TestCaseData.ksParam_sBaseCmd,         oData.sBaseCmd,         'Base command')
        oForm.addText       (TestCaseData.ksParam_sValidationKitZips,   oData.sValidationKitZips,   'Test suite files')

        oForm.addListOfTestCaseArgs(TestCaseDataEx.ksParam_aoTestCaseArgs, oData.aoTestCaseArgs, 'Argument variations')

        aoTestCaseDeps = [];
        for oTestCase in self._aoAllTestCases:
            if oTestCase.idTestCase == oData.idTestCase:
                continue;
            fSelected = False;
            for oDep in oData.aoDepTestCases:
                if oDep.idTestCase == oTestCase.idTestCase:
                    fSelected = True;
                    break;
            aoTestCaseDeps.append([oTestCase.idTestCase, fSelected, oTestCase.sName]);
        oForm.addListOfTestCases(TestCaseDataEx.ksParam_aoDepTestCases, aoTestCaseDeps,     'Depends on test cases')

        aoGlobalResrcDeps = [];
        for oGlobalRsrc in self._aoAllGlobalRsrcs:
            fSelected = False;
            for oDep in oData.aoDepGlobalResources:
                if oDep.idGlobalRsrc == oGlobalRsrc.idGlobalRsrc:
                    fSelected = True;
                    break;
            aoGlobalResrcDeps.append([oGlobalRsrc.idGlobalRsrc, fSelected, oGlobalRsrc.sName]);
        oForm.addListOfResources(TestCaseDataEx.ksParam_aoDepGlobalResources, aoGlobalResrcDeps, 'Depends on resources')

        oForm.addSubmit();

        return True;

