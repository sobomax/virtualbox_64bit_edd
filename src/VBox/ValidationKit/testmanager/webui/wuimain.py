# -*- coding: utf-8 -*-
# $Id: wuimain.py $

"""
Test Manager Core - WUI - The Main page.
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
__version__ = "$Revision: 101459 $"

# Standard Python imports.

# Validation Kit imports.
from testmanager                            import config;
from testmanager.webui.wuibase              import WuiDispatcherBase, WuiException;
from testmanager.webui.wuicontentbase       import WuiTmLink;
from testmanager.core.report                import ReportLazyModel, ReportGraphModel, ReportModelBase;
from testmanager.core.testresults           import TestResultLogic, TestResultFileDataEx;
from testmanager.core.base                  import TMExceptionBase, TMTooManyRows;
from testmanager.core.testset               import TestSetData, TestSetLogic;
from testmanager.core.build                 import BuildDataEx;
from testmanager.core.testbox               import TestBoxData
from testmanager.core.testgroup             import TestGroupData;
from testmanager.core.testcase              import TestCaseDataEx
from testmanager.core.testcaseargs          import TestCaseArgsDataEx
from testmanager.core.vcsrevisions          import VcsRevisionLogic;
from common                                 import webutils, utils;


class WuiMain(WuiDispatcherBase):
    """
    WUI Main page.

    Note! All cylic dependency avoiance stuff goes here in the dispatcher code,
          not in the action specific code.  This keeps the uglyness in one place
          and reduces load time dependencies in the more critical code path.
    """

    ## The name of the script.
    ksScriptName = 'index.py'

    ## @name Actions
    ## @{
    ksActionResultsUnGrouped            = 'ResultsUnGrouped'
    ksActionResultsGroupedBySchedGroup  = 'ResultsGroupedBySchedGroup'
    ksActionResultsGroupedByTestGroup   = 'ResultsGroupedByTestGroup'
    ksActionResultsGroupedByBuildRev    = 'ResultsGroupedByBuildRev'
    ksActionResultsGroupedByTestBox     = 'ResultsGroupedByTestBox'
    ksActionResultsGroupedByTestCase    = 'ResultsGroupedByTestCase'
    ksActionTestResultDetails           = 'TestResultDetails'
    ksActionViewLog                     = 'ViewLog'
    ksActionGetFile                     = 'GetFile'
    ksActionReportSummary               = 'ReportSummary';
    ksActionReportRate                  = 'ReportRate';
    ksActionReportFailureReasons        = 'ReportFailureReasons';
    ksActionGraphWiz                    = 'GraphWiz';
    ksActionVcsHistoryTooltip           = 'VcsHistoryTooltip';
    ## @}

    ## @name Standard report parameters
    ## @{
    ksParamReportPeriods        = 'cPeriods';
    ksParamReportPeriodInHours  = 'cHoursPerPeriod';
    ksParamReportSubject        = 'sSubject';
    ksParamReportSubjectIds     = 'SubjectIds';
    ## @}

    ## @name Graph Wizard parameters
    ## Common parameters: ksParamReportPeriods, ksParamReportPeriodInHours, ksParamReportSubjectIds,
    ##                    ksParamReportSubject, ksParamEffectivePeriod, and ksParamEffectiveDate.
    ## @{
    ksParamGraphWizTestBoxIds   = 'aidTestBoxes';
    ksParamGraphWizBuildCatIds  = 'aidBuildCats';
    ksParamGraphWizTestCaseIds  = 'aidTestCases';
    ksParamGraphWizSepTestVars  = 'fSepTestVars';
    ksParamGraphWizImpl         = 'enmImpl';
    ksParamGraphWizWidth        = 'cx';
    ksParamGraphWizHeight       = 'cy';
    ksParamGraphWizDpi          = 'dpi';
    ksParamGraphWizFontSize     = 'cPtFont';
    ksParamGraphWizErrorBarY    = 'fErrorBarY';
    ksParamGraphWizMaxErrorBarY = 'cMaxErrorBarY';
    ksParamGraphWizMaxPerGraph  = 'cMaxPerGraph';
    ksParamGraphWizXkcdStyle    = 'fXkcdStyle';
    ksParamGraphWizTabular      = 'fTabular';
    ksParamGraphWizSrcTestSetId = 'idSrcTestSet';
    ## @}

    ## @name Graph implementations values for ksParamGraphWizImpl.
    ## @{
    ksGraphWizImpl_Default      = 'default';
    ksGraphWizImpl_Matplotlib   = 'matplotlib';
    ksGraphWizImpl_Charts       = 'charts';
    kasGraphWizImplValid        = [ ksGraphWizImpl_Default, ksGraphWizImpl_Matplotlib, ksGraphWizImpl_Charts];
    kaasGraphWizImplCombo       = [
        ( ksGraphWizImpl_Default,       'Default' ),
        ( ksGraphWizImpl_Matplotlib,    'Matplotlib (server)' ),
        ( ksGraphWizImpl_Charts,        'Google Charts (client)'),
    ];
    ## @}

    ## @name Log Viewer parameters.
    ## @{
    ksParamLogSetId             = 'LogViewer_idTestSet';
    ksParamLogFileId            = 'LogViewer_idFile';
    ksParamLogChunkSize         = 'LogViewer_cbChunk';
    ksParamLogChunkNo           = 'LogViewer_iChunk';
    ## @}

    ## @name File getter parameters.
    ## @{
    ksParamGetFileSetId         = 'GetFile_idTestSet';
    ksParamGetFileId            = 'GetFile_idFile';
    ksParamGetFileDownloadIt    = 'GetFile_fDownloadIt';
    ## @}

    ## @name VCS history parameters.
    ## @{
    ksParamVcsHistoryRepository = 'repo';
    ksParamVcsHistoryRevision   = 'rev';
    ksParamVcsHistoryEntries    = 'cEntries';
    ## @}

    ## @name Test result listing parameters.
    ## @{
    ## If this param is specified, then show only results for this member when results grouped by some parameter.
    ksParamGroupMemberId        = 'GroupMemberId'
    ## Optional parameter for indicating whether to restrict the listing to failures only.
    ksParamOnlyFailures         = 'OnlyFailures'
    ## Result listing sorting.
    ksParamTestResultsSortBy    = 'enmSortBy'
    ## @}

    ## Effective time period. one of the first column values in kaoResultPeriods.
    ksParamEffectivePeriod      = 'sEffectivePeriod'

    ## Test result period values.
    kaoResultPeriods = [
        ( '1 hour',   'One hour',    1 ),
        ( '2 hours',  'Two hours',   2 ),
        ( '3 hours',  'Three hours', 3 ),
        ( '6 hours',  'Six hours',   6 ),
        ( '12 hours', '12 hours',    12 ),

        ( '1 day',    'One day',     24 ),
        ( '2 days',   'Two days',    48 ),
        ( '3 days',   'Three days',  72 ),

        ( '1 week',   'One week',    168 ),
        ( '2 weeks',  'Two weeks',   336 ),
        ( '3 weeks',  'Three weeks', 504 ),

        ( '1 month',  'One month',   31 * 24 ),                             # The approx hour count varies with the start date.
        ( '2 months', 'Two month',   (31 + 31) * 24 ),                      # Using maximum values.
        ( '3 months', 'Three month', (31 + 30 + 31) * 24 ),

        ( '6 months', 'Six month',   (31 + 31 + 30 + 31 + 30 + 31) * 24 ),

        ( '1 year',   'One year',    365 * 24 ),
    ];
    ## The default test result period.
    ksResultPeriodDefault = '6 hours';



    def __init__(self, oSrvGlue):
        WuiDispatcherBase.__init__(self, oSrvGlue, self.ksScriptName);

        self._sTemplate     = 'template.html'

        #
        # Populate the action dispatcher dictionary.
        #

        # Use short form to avoid hitting the right margin (130) when using lambda.
        d = self._dDispatch;  # pylint: disable=C0103

        from testmanager.webui.wuitestresult import WuiGroupedResultList;
        #d[self.ksActionResultsUnGrouped]          = lambda: self._actionResultsListing(TestResultLogic, WuiGroupedResultList)
        d[self.ksActionResultsUnGrouped]          = lambda: self._actionGroupedResultsListing(
                                                                TestResultLogic.ksResultsGroupingTypeNone,
                                                                TestResultLogic,
                                                                WuiGroupedResultList)

        d[self.ksActionResultsGroupedByTestGroup] = lambda: self._actionGroupedResultsListing(
                                                                TestResultLogic.ksResultsGroupingTypeTestGroup,
                                                                TestResultLogic,
                                                                WuiGroupedResultList)

        d[self.ksActionResultsGroupedByBuildRev]  = lambda: self._actionGroupedResultsListing(
                                                                TestResultLogic.ksResultsGroupingTypeBuildRev,
                                                                TestResultLogic,
                                                                WuiGroupedResultList)

        d[self.ksActionResultsGroupedByTestBox]   = lambda: self._actionGroupedResultsListing(
                                                                TestResultLogic.ksResultsGroupingTypeTestBox,
                                                                TestResultLogic,
                                                                WuiGroupedResultList)

        d[self.ksActionResultsGroupedByTestCase]   = lambda: self._actionGroupedResultsListing(
                                                                TestResultLogic.ksResultsGroupingTypeTestCase,
                                                                TestResultLogic,
                                                                WuiGroupedResultList)

        d[self.ksActionResultsGroupedBySchedGroup] = lambda: self._actionGroupedResultsListing(
                                                                TestResultLogic.ksResultsGroupingTypeSchedGroup,
                                                                TestResultLogic,
                                                                WuiGroupedResultList)

        d[self.ksActionTestResultDetails]          = self.actionTestResultDetails

        d[self.ksActionViewLog]                 = self.actionViewLog;
        d[self.ksActionGetFile]                 = self.actionGetFile;
        from testmanager.webui.wuireport import WuiReportSummary, WuiReportSuccessRate, WuiReportFailureReasons;
        d[self.ksActionReportSummary]           = lambda: self._actionGenericReport(ReportLazyModel, WuiReportSummary);
        d[self.ksActionReportRate]              = lambda: self._actionGenericReport(ReportLazyModel, WuiReportSuccessRate);
        d[self.ksActionReportFailureReasons]    = lambda: self._actionGenericReport(ReportLazyModel, WuiReportFailureReasons);
        d[self.ksActionGraphWiz]                = self._actionGraphWiz;
        d[self.ksActionVcsHistoryTooltip]       = self._actionVcsHistoryTooltip;


        #
        # Popupate the menus.
        #

        # Additional URL parameters keeping for time navigation.
        sExtraTimeNav = ''
        dCurParams = oSrvGlue.getParameters()
        if dCurParams is not None:
            asActionUrlExtras = [ self.ksParamItemsPerPage, self.ksParamEffectiveDate, self.ksParamEffectivePeriod, ];
            for sExtraParam in asActionUrlExtras:
                if sExtraParam in dCurParams:
                    sExtraTimeNav += '&%s' % webutils.encodeUrlParams({sExtraParam: dCurParams[sExtraParam]})

        # Shorthand to keep within margins.
        sActUrlBase = self._sActionUrlBase;

        self._aaoMenus = \
        [
            [
                'Inbox',            sActUrlBase + 'TODO', ## @todo list of failures that needs categorizing.
                []
            ],
            [
                'Reports',          sActUrlBase + self.ksActionReportSummary,
                [
                    [ 'Summary',                  sActUrlBase + self.ksActionReportSummary ],
                    [ 'Success Rate',             sActUrlBase + self.ksActionReportRate ],
                    [ 'Failure Reasons',          sActUrlBase + self.ksActionReportFailureReasons ],
                ]
            ],
            [
                'Test Results',     sActUrlBase + self.ksActionResultsUnGrouped + sExtraTimeNav,
                [
                    [ 'Ungrouped results',           sActUrlBase + self.ksActionResultsUnGrouped           + sExtraTimeNav ],
                    [ 'Grouped by Scheduling Group', sActUrlBase + self.ksActionResultsGroupedBySchedGroup + sExtraTimeNav ],
                    [ 'Grouped by Test Group',       sActUrlBase + self.ksActionResultsGroupedByTestGroup  + sExtraTimeNav ],
                    [ 'Grouped by TestBox',          sActUrlBase + self.ksActionResultsGroupedByTestBox    + sExtraTimeNav ],
                    [ 'Grouped by Test Case',        sActUrlBase + self.ksActionResultsGroupedByTestCase   + sExtraTimeNav ],
                    [ 'Grouped by Revision',         sActUrlBase + self.ksActionResultsGroupedByBuildRev   + sExtraTimeNav ],
                ]
            ],
            [
                '> Admin', 'admin.py?' + webutils.encodeUrlParams(self._dDbgParams), []
            ],
        ];


    def _actionDefault(self):
        """Show the default admin page."""
        from testmanager.webui.wuitestresult import WuiGroupedResultList;
        self._sAction = self.ksActionResultsUnGrouped
        return self._actionGroupedResultsListing(TestResultLogic.ksResultsGroupingTypeNone,
                                                 TestResultLogic,
                                                 WuiGroupedResultList)


    #
    # Navigation bar stuff
    #

    def _generateSortBySelector(self, dParams, sPreamble, sPostamble):
        """
        Generate HTML code for the sort by selector.
        """
        if self.ksParamTestResultsSortBy in dParams:
            enmResultSortBy = dParams[self.ksParamTestResultsSortBy];
            del dParams[self.ksParamTestResultsSortBy];
        else:
            enmResultSortBy = TestResultLogic.ksResultsSortByRunningAndStart;

        sHtmlSortBy  = '<form name="TimeForm" method="GET"> Sort by\n';
        sHtmlSortBy += sPreamble;
        sHtmlSortBy += '\n  <select name="%s" onchange="window.location=' % (self.ksParamTestResultsSortBy,);
        sHtmlSortBy += '\'?%s&%s=\' + ' % (webutils.encodeUrlParams(dParams), self.ksParamTestResultsSortBy)
        sHtmlSortBy += 'this.options[this.selectedIndex].value;" title="Sorting by">\n'

        fSelected = False;
        for enmCode, sTitle in TestResultLogic.kaasResultsSortByTitles:
            if enmCode == enmResultSortBy:
                fSelected = True;
            sHtmlSortBy += '    <option value="%s"%s>%s</option>\n' \
                         % (enmCode, ' selected="selected"' if enmCode == enmResultSortBy else '', sTitle,);
        assert fSelected;
        sHtmlSortBy += '  </select>\n';
        sHtmlSortBy += sPostamble;
        sHtmlSortBy += '\n</form>\n'
        return sHtmlSortBy;

    def _generateStatusSelector(self, dParams, fOnlyFailures):
        """
        Generate HTML code for the status code selector.  Currently very simple.
        """
        dParams[self.ksParamOnlyFailures] = not fOnlyFailures;
        return WuiTmLink('Show all results' if fOnlyFailures else 'Only show failed tests', '', dParams,
                         fBracketed = False).toHtml();

    def _generateTimeSelector(self, dParams, sPreamble, sPostamble):
        """
        Generate HTML code for time selector.
        """

        if WuiDispatcherBase.ksParamEffectiveDate in dParams:
            tsEffective = dParams[WuiDispatcherBase.ksParamEffectiveDate]
            del dParams[WuiDispatcherBase.ksParamEffectiveDate]
        else:
            tsEffective = ''

        # Forget about page No when changing a period
        if WuiDispatcherBase.ksParamPageNo in dParams:
            del dParams[WuiDispatcherBase.ksParamPageNo]

        sHtmlTimeSelector  = '<form name="TimeForm" method="GET">\n'
        sHtmlTimeSelector += sPreamble;
        sHtmlTimeSelector += '\n  <select name="%s" onchange="window.location=' % WuiDispatcherBase.ksParamEffectiveDate
        sHtmlTimeSelector += '\'?%s&%s=\' + ' % (webutils.encodeUrlParams(dParams), WuiDispatcherBase.ksParamEffectiveDate)
        sHtmlTimeSelector += 'this.options[this.selectedIndex].value;" title="Effective date">\n'

        aoWayBackPoints = [
            ('+0000-00-00 00:00:00.00', 'Now', ' title="Present Day. Present Time."'), # lain :)

            ('-0000-00-00 01:00:00.00', 'One hour ago', ''),
            ('-0000-00-00 02:00:00.00', 'Two hours ago', ''),
            ('-0000-00-00 03:00:00.00', 'Three hours ago', ''),

            ('-0000-00-01 00:00:00.00', 'One day ago', ''),
            ('-0000-00-02 00:00:00.00', 'Two days ago', ''),
            ('-0000-00-03 00:00:00.00', 'Three days ago', ''),

            ('-0000-00-07 00:00:00.00', 'One week ago', ''),
            ('-0000-00-14 00:00:00.00', 'Two weeks ago', ''),
            ('-0000-00-21 00:00:00.00', 'Three weeks ago', ''),

            ('-0000-01-00 00:00:00.00', 'One month ago', ''),
            ('-0000-02-00 00:00:00.00', 'Two months ago', ''),
            ('-0000-03-00 00:00:00.00', 'Three months ago', ''),
            ('-0000-04-00 00:00:00.00', 'Four months ago', ''),
            ('-0000-05-00 00:00:00.00', 'Five months ago', ''),
            ('-0000-06-00 00:00:00.00', 'Half a year ago', ''),

            ('-0001-00-00 00:00:00.00', 'One year ago', ''),
        ]
        fSelected = False;
        for sTimestamp, sWayBackPointCaption, sExtraAttrs in aoWayBackPoints:
            if sTimestamp == tsEffective:
                fSelected = True;
            sHtmlTimeSelector += '    <option value="%s"%s%s>%s</option>\n' \
                              % (webutils.quoteUrl(sTimestamp),
                                 ' selected="selected"' if sTimestamp == tsEffective else '',
                                 sExtraAttrs, sWayBackPointCaption)
        if not fSelected and tsEffective != '':
            sHtmlTimeSelector += '    <option value="%s" selected>%s</option>\n' \
                              % (webutils.quoteUrl(tsEffective), tsEffective)

        sHtmlTimeSelector += '  </select>\n';
        sHtmlTimeSelector += sPostamble;
        sHtmlTimeSelector += '\n</form>\n'

        return sHtmlTimeSelector

    def _generateTimeWalker(self, dParams, tsEffective, sCurPeriod):
        """
        Generates HTML code for walking back and forth in time.
        """
        # Have to do some math here. :-/
        if tsEffective is None:
            self._oDb.execute('SELECT CURRENT_TIMESTAMP - \'' + sCurPeriod + '\'::interval');
            tsNext = None;
            tsPrev = self._oDb.fetchOne()[0];
        else:
            self._oDb.execute('SELECT %s::TIMESTAMP - \'' + sCurPeriod + '\'::interval,\n'
                              '       %s::TIMESTAMP + \'' + sCurPeriod + '\'::interval',
                              (tsEffective, tsEffective,));
            tsPrev, tsNext = self._oDb.fetchOne();

        # Forget about page No when changing a period
        if WuiDispatcherBase.ksParamPageNo in dParams:
            del dParams[WuiDispatcherBase.ksParamPageNo]

        # Format.
        dParams[WuiDispatcherBase.ksParamEffectiveDate] = str(tsPrev);
        sPrev = '<a href="?%s" title="One period earlier">&lt;&lt;</a>&nbsp;&nbsp;' \
              % (webutils.encodeUrlParams(dParams),);

        if tsNext is not None:
            dParams[WuiDispatcherBase.ksParamEffectiveDate] = str(tsNext);
            sNext = '&nbsp;&nbsp;<a href="?%s" title="One period later">&gt;&gt;</a>' \
                   % (webutils.encodeUrlParams(dParams),);
        else:
            sNext = '&nbsp;&nbsp;&gt;&gt;';

        return self._generateTimeSelector(self.getParameters(), sPrev, sNext);

    def _generateResultPeriodSelector(self, dParams, sCurPeriod):
        """
        Generate HTML code for result period selector.
        """

        if self.ksParamEffectivePeriod in dParams:
            del dParams[self.ksParamEffectivePeriod];

        # Forget about page No when changing a period
        if WuiDispatcherBase.ksParamPageNo in dParams:
            del dParams[WuiDispatcherBase.ksParamPageNo]

        sHtmlPeriodSelector  = '<form name="PeriodForm" method="GET">\n'
        sHtmlPeriodSelector += '  Period is\n'
        sHtmlPeriodSelector += '  <select name="%s" onchange="window.location=' % self.ksParamEffectivePeriod
        sHtmlPeriodSelector += '\'?%s&%s=\' + ' % (webutils.encodeUrlParams(dParams), self.ksParamEffectivePeriod)
        sHtmlPeriodSelector += 'this.options[this.selectedIndex].value;">\n'

        for sPeriodValue, sPeriodCaption, _ in self.kaoResultPeriods:
            sHtmlPeriodSelector += '    <option value="%s"%s>%s</option>\n' \
                                % (webutils.quoteUrl(sPeriodValue),
                                   ' selected="selected"' if sPeriodValue == sCurPeriod else '',
                                   sPeriodCaption)

        sHtmlPeriodSelector += '  </select>\n' \
                               '</form>\n'

        return sHtmlPeriodSelector

    def _generateGroupContentSelector(self, aoGroupMembers, iCurrentMember, sAltAction):
        """
        Generate HTML code for group content selector.
        """

        dParams = self.getParameters()

        if self.ksParamGroupMemberId in dParams:
            del dParams[self.ksParamGroupMemberId]

        if sAltAction is not None:
            if self.ksParamAction in dParams:
                del dParams[self.ksParamAction];
            dParams[self.ksParamAction] = sAltAction;

        sHtmlSelector  = '<form name="GroupContentForm" method="GET">\n'
        sHtmlSelector += '  <select name="%s" onchange="window.location=' % self.ksParamGroupMemberId
        sHtmlSelector += '\'?%s&%s=\' + ' % (webutils.encodeUrlParams(dParams), self.ksParamGroupMemberId)
        sHtmlSelector += 'this.options[this.selectedIndex].value;">\n'

        sHtmlSelector += '<option value="-1">All</option>\n'

        for iGroupMemberId, sGroupMemberName in aoGroupMembers:
            if iGroupMemberId is not None:
                sHtmlSelector += '    <option value="%s"%s>%s</option>\n' \
                                    % (iGroupMemberId,
                                       ' selected="selected"' if iGroupMemberId == iCurrentMember else '',
                                       sGroupMemberName)

        sHtmlSelector += '  </select>\n' \
                         '</form>\n'

        return sHtmlSelector

    def _generatePagesSelector(self, dParams, cItems, cItemsPerPage, iPage):
        """
        Generate HTML code for pages (1, 2, 3 ... N) selector
        """

        if WuiDispatcherBase.ksParamPageNo in dParams:
            del dParams[WuiDispatcherBase.ksParamPageNo]

        sHrefPtr    = '<a href="?%s&%s=' % (webutils.encodeUrlParams(dParams).replace('%', '%%'),
                                            WuiDispatcherBase.ksParamPageNo)
        sHrefPtr   += '%d">%s</a>'

        cNumOfPages      = (cItems + cItemsPerPage - 1) / cItemsPerPage;
        cPagesToDisplay  = 10
        cPagesRangeStart = iPage - cPagesToDisplay / 2 \
                           if not iPage - cPagesToDisplay / 2 < 0 else 0
        cPagesRangeEnd   = cPagesRangeStart + cPagesToDisplay \
                           if not cPagesRangeStart + cPagesToDisplay > cNumOfPages else cNumOfPages
        # Adjust pages range
        if cNumOfPages < cPagesToDisplay:
            cPagesRangeStart = 0
            cPagesRangeEnd   = cNumOfPages

        # 1 2 3 4...
        sHtmlPager  = '&nbsp;\n'.join(sHrefPtr % (x, str(x + 1)) if x != iPage else str(x + 1)
                                      for x in range(cPagesRangeStart, cPagesRangeEnd))
        if cPagesRangeStart > 0:
            sHtmlPager = '%s&nbsp; ... &nbsp;\n' % (sHrefPtr % (0, str(1))) + sHtmlPager
        if cPagesRangeEnd < cNumOfPages:
            sHtmlPager += ' ... %s\n' % (sHrefPtr % (cNumOfPages, str(cNumOfPages + 1)))

        # Prev/Next (using << >> because &laquo; and &raquo are too tiny).
        if iPage > 0:
            dParams[WuiDispatcherBase.ksParamPageNo] = iPage - 1
            sHtmlPager = ('<a title="Previous page" href="?%s">&lt;&lt;</a>&nbsp;&nbsp;\n'
                          % (webutils.encodeUrlParams(dParams), )) \
                          + sHtmlPager;
        else:
            sHtmlPager = '&lt;&lt;&nbsp;&nbsp;\n' + sHtmlPager

        if iPage + 1 < cNumOfPages:
            dParams[WuiDispatcherBase.ksParamPageNo] = iPage + 1
            sHtmlPager += '\n&nbsp; <a title="Next page" href="?%s">&gt;&gt;</a>\n' % (webutils.encodeUrlParams(dParams),)
        else:
            sHtmlPager += '\n&nbsp; &gt;&gt;\n'

        return sHtmlPager

    def _generateItemPerPageSelector(self, dParams, cItemsPerPage):
        """
        Generate HTML code for items per page selector
        """

        if WuiDispatcherBase.ksParamItemsPerPage in dParams:
            del dParams[WuiDispatcherBase.ksParamItemsPerPage]

        # Forced reset of the page number
        dParams[WuiDispatcherBase.ksParamPageNo] = 0
        sHtmlItemsPerPageSelector  = '<form name="AgesPerPageForm" method="GET">\n' \
                                     '  Max <select name="%s" onchange="window.location=\'?%s&%s=\' + ' \
                                     'this.options[this.selectedIndex].value;" title="Max items per page">\n' \
                                   % (WuiDispatcherBase.ksParamItemsPerPage,
                                      webutils.encodeUrlParams(dParams),
                                      WuiDispatcherBase.ksParamItemsPerPage)

        aiItemsPerPage = [16, 32, 64, 128, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096];
        for iItemsPerPage in aiItemsPerPage:
            sHtmlItemsPerPageSelector += '    <option value="%d" %s>%d</option>\n' \
                                       % (iItemsPerPage,
                                          'selected="selected"' if iItemsPerPage == cItemsPerPage else '',
                                          iItemsPerPage)
        sHtmlItemsPerPageSelector += '  </select> items per page\n' \
                                     '</form>\n'

        return sHtmlItemsPerPageSelector

    def _generateResultNavigation(self, cItems, cItemsPerPage, iPage, tsEffective, sCurPeriod, fOnlyFailures,
                                  sHtmlMemberSelector):
        """ Make custom time navigation bar for the results. """

        # Generate the elements.
        sHtmlStatusSelector = self._generateStatusSelector(self.getParameters(), fOnlyFailures);
        sHtmlSortBySelector = self._generateSortBySelector(self.getParameters(), '', sHtmlStatusSelector);
        sHtmlPeriodSelector = self._generateResultPeriodSelector(self.getParameters(), sCurPeriod)
        sHtmlTimeWalker     = self._generateTimeWalker(self.getParameters(), tsEffective, sCurPeriod);

        if cItems > 0:
            sHtmlPager = self._generatePagesSelector(self.getParameters(), cItems, cItemsPerPage, iPage)
            sHtmlItemsPerPageSelector = self._generateItemPerPageSelector(self.getParameters(), cItemsPerPage)
        else:
            sHtmlPager = ''
            sHtmlItemsPerPageSelector = ''

        # Generate navigation bar
        sHtml = '<table width=100%>\n' \
                '<tr>\n' \
                ' <td width=30%>' + sHtmlMemberSelector + '</td>\n' \
                ' <td width=40% align=center>' + sHtmlTimeWalker + '</td>' \
                ' <td width=30% align=right>\n' + sHtmlPeriodSelector + '</td>\n' \
                '</tr>\n' \
                '<tr>\n' \
                ' <td width=30%>' + sHtmlSortBySelector + '</td>\n' \
                ' <td width=40% align=center>\n' + sHtmlPager + '</td>\n' \
                ' <td width=30% align=right>\n' + sHtmlItemsPerPageSelector + '</td>\n'\
                '</tr>\n' \
                '</table>\n'

        return sHtml

    def _generateReportNavigation(self, tsEffective, cHoursPerPeriod, cPeriods):
        """ Make time navigation bar for the reports. """

        # The period length selector.
        dParams = self.getParameters();
        if WuiMain.ksParamReportPeriodInHours in dParams:
            del dParams[WuiMain.ksParamReportPeriodInHours];
        sHtmlPeriodLength  = '';
        sHtmlPeriodLength += '<form name="ReportPeriodInHoursForm" method="GET">\n' \
                             '  Period length <select name="%s" onchange="window.location=\'?%s&%s=\' + ' \
                             'this.options[this.selectedIndex].value;" title="Statistics period length in hours.">\n' \
                           % (WuiMain.ksParamReportPeriodInHours,
                              webutils.encodeUrlParams(dParams),
                              WuiMain.ksParamReportPeriodInHours)
        for cHours in [ 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 18, 24, 48, 72, 96, 120, 144, 168 ]:
            sHtmlPeriodLength += '    <option value="%d"%s>%d hour%s</option>\n' \
                               % (cHours, 'selected="selected"' if cHours == cHoursPerPeriod else '', cHours,
                                  's' if cHours > 1 else '');
        sHtmlPeriodLength += '  </select>\n' \
                             '</form>\n'

        # The period count selector.
        dParams = self.getParameters();
        if WuiMain.ksParamReportPeriods in dParams:
            del dParams[WuiMain.ksParamReportPeriods];
        sHtmlCountOfPeriods  = '';
        sHtmlCountOfPeriods += '<form name="ReportPeriodsForm" method="GET">\n' \
                               '  Periods <select name="%s" onchange="window.location=\'?%s&%s=\' + ' \
                               'this.options[this.selectedIndex].value;" title="Statistics periods to report.">\n' \
                             % (WuiMain.ksParamReportPeriods,
                                webutils.encodeUrlParams(dParams),
                                WuiMain.ksParamReportPeriods)
        for cCurPeriods in range(2, 43):
            sHtmlCountOfPeriods += '    <option value="%d"%s>%d</option>\n' \
                                 % (cCurPeriods, 'selected="selected"' if cCurPeriods == cPeriods else '', cCurPeriods);
        sHtmlCountOfPeriods += '  </select>\n' \
                               '</form>\n'

        # The time walker.
        sHtmlTimeWalker = self._generateTimeWalker(self.getParameters(), tsEffective, '%d hours' % (cHoursPerPeriod));

        # Combine them all.
        sHtml = '<table width=100%>\n' \
                ' <tr>\n' \
                '  <td width=30% align="center">\n' + sHtmlPeriodLength + '</td>\n' \
                '  <td width=40% align="center">\n' + sHtmlTimeWalker + '</td>' \
                '  <td width=30% align="center">\n' + sHtmlCountOfPeriods + '</td>\n' \
                ' </tr>\n' \
                '</table>\n';
        return sHtml;

    #
    # The rest of stuff
    #

    def _actionGroupedResultsListing( #pylint: disable=R0914
            self,
            enmResultsGroupingType,
            oResultsLogicType,
            oResultsListContentType):
        """
        Override generic listing action.

        oLogicType implements fetchForListing.
        oListContentType is a child of WuiListContentBase.
        """
        cItemsPerPage     = self.getIntParam(self.ksParamItemsPerPage,  iMin =  2, iMax =   9999, iDefault = 128);
        iPage             = self.getIntParam(self.ksParamPageNo,        iMin =  0, iMax = 999999, iDefault = 0);
        tsEffective       = self.getEffectiveDateParam();
        iGroupMemberId    = self.getIntParam(self.ksParamGroupMemberId, iMin = -1, iMax = 999999, iDefault = -1);
        fOnlyFailures     = self.getBoolParam(self.ksParamOnlyFailures, fDefault = False);
        enmResultSortBy   = self.getStringParam(self.ksParamTestResultsSortBy,
                                                asValidValues = TestResultLogic.kasResultsSortBy,
                                                sDefault = TestResultLogic.ksResultsSortByRunningAndStart);

        # Get testing results period and validate it
        asValidValues     = [x for (x, _, _) in self.kaoResultPeriods]
        sCurPeriod        = self.getStringParam(self.ksParamEffectivePeriod, asValidValues = asValidValues,
                                                sDefault = self.ksResultPeriodDefault)
        assert sCurPeriod != ''; # Impossible!

        self._checkForUnknownParameters()

        #
        # Fetch the group members.
        #
        # If no grouping is selected, we'll fill the the grouping combo with
        # testboxes just to avoid having completely useless combo box.
        #
        oTrLogic = TestResultLogic(self._oDb);
        sAltSelectorAction = None;
        if   enmResultsGroupingType == TestResultLogic.ksResultsGroupingTypeNone \
          or enmResultsGroupingType == TestResultLogic.ksResultsGroupingTypeTestBox:
            aoTmp = oTrLogic.getTestBoxes(tsNow = tsEffective, sPeriod = sCurPeriod)
            aoGroupMembers = sorted(list(set([ (x.idTestBox, '%s (%s)' % (x.sName, str(x.ip))) for x in aoTmp ])),
                                    reverse = False, key = lambda asData: asData[1])

            if enmResultsGroupingType == TestResultLogic.ksResultsGroupingTypeTestBox:
                self._sPageTitle = 'Grouped by Test Box';
            else:
                self._sPageTitle = 'Ungrouped results';
                sAltSelectorAction = self.ksActionResultsGroupedByTestBox;
                aoGroupMembers.insert(0, [None, None]); # The "All" member.

        elif enmResultsGroupingType == TestResultLogic.ksResultsGroupingTypeTestGroup:
            aoTmp = oTrLogic.getTestGroups(tsNow = tsEffective, sPeriod = sCurPeriod);
            aoGroupMembers = sorted(list(set([ (x.idTestGroup, x.sName ) for x in aoTmp ])),
                                    reverse = False, key = lambda asData: asData[1])
            self._sPageTitle = 'Grouped by Test Group'

        elif enmResultsGroupingType == TestResultLogic.ksResultsGroupingTypeBuildRev:
            aoTmp = oTrLogic.getBuilds(tsNow = tsEffective, sPeriod = sCurPeriod)
            aoGroupMembers = sorted(list(set([ (x.iRevision, '%s.%d' % (x.oCat.sBranch, x.iRevision)) for x in aoTmp ])),
                                    reverse = True, key = lambda asData: asData[0])
            self._sPageTitle = 'Grouped by Build'

        elif enmResultsGroupingType == TestResultLogic.ksResultsGroupingTypeTestCase:
            aoTmp = oTrLogic.getTestCases(tsNow = tsEffective, sPeriod = sCurPeriod)
            aoGroupMembers = sorted(list(set([ (x.idTestCase, '%s' % x.sName) for x in aoTmp ])),
                                    reverse = False, key = lambda asData: asData[1])
            self._sPageTitle = 'Grouped by Test Case'

        elif enmResultsGroupingType == TestResultLogic.ksResultsGroupingTypeSchedGroup:
            aoTmp = oTrLogic.getSchedGroups(tsNow = tsEffective, sPeriod = sCurPeriod)
            aoGroupMembers = sorted(list(set([ (x.idSchedGroup, '%s' % x.sName) for x in aoTmp ])),
                                    reverse = False, key = lambda asData: asData[1])
            self._sPageTitle = 'Grouped by Scheduling Group'

        else:
            raise TMExceptionBase('Unknown grouping type')

        _sPageBody  = ''
        oContent    = None
        cEntriesMax = 0
        _dParams    = self.getParameters()
        for idMember, sMemberName in aoGroupMembers:
            #
            # Count and fetch entries to be displayed.
            #

            # Skip group members that were not specified.
            if    idMember != iGroupMemberId \
              and (   (idMember is not None and enmResultsGroupingType == TestResultLogic.ksResultsGroupingTypeNone)
                   or (iGroupMemberId > 0   and enmResultsGroupingType != TestResultLogic.ksResultsGroupingTypeNone) ):
                continue

            oResultLogic = oResultsLogicType(self._oDb);
            cEntries = oResultLogic.getEntriesCount(tsNow = tsEffective,
                                                    sInterval = sCurPeriod,
                                                    enmResultsGroupingType = enmResultsGroupingType,
                                                    iResultsGroupingValue = idMember,
                                                    fOnlyFailures = fOnlyFailures);
            if cEntries == 0: # Do not display empty groups
                continue
            aoEntries = oResultLogic.fetchResultsForListing(iPage * cItemsPerPage,
                                                            cItemsPerPage,
                                                            tsNow = tsEffective,
                                                            sInterval = sCurPeriod,
                                                            enmResultSortBy = enmResultSortBy,
                                                            enmResultsGroupingType = enmResultsGroupingType,
                                                            iResultsGroupingValue = idMember,
                                                            fOnlyFailures = fOnlyFailures);
            cEntriesMax = max(cEntriesMax, cEntries)

            #
            # Format them.
            #
            oContent = oResultsListContentType(aoEntries,
                                               cEntries,
                                               iPage,
                                               cItemsPerPage,
                                               tsEffective,
                                               fnDPrint = self._oSrvGlue.dprint,
                                               oDisp = self)

            (_, sHtml) = oContent.show(fShowNavigation = False)
            if sMemberName is not None:
                _sPageBody += '<table width=100%><tr><td>'

                _dParams[self.ksParamGroupMemberId] = idMember
                sLink = WuiTmLink(sMemberName, '', _dParams, fBracketed = False).toHtml()

                _sPageBody += '<h2>%s (%d)</h2></td>' % (sLink, cEntries)
                _sPageBody += '<td><br></td>'
                _sPageBody += '</tr></table>'
            _sPageBody += sHtml
            _sPageBody += '<br>'

        #
        # Complete the page by slapping navigation controls at the top and
        # bottom of it.
        #
        sHtmlNavigation = self._generateResultNavigation(cEntriesMax, cItemsPerPage, iPage,
                                                         tsEffective, sCurPeriod, fOnlyFailures,
                                                         self._generateGroupContentSelector(aoGroupMembers, iGroupMemberId,
                                                                                            sAltSelectorAction));
        if cEntriesMax > 0:
            self._sPageBody = sHtmlNavigation + _sPageBody + sHtmlNavigation;
        else:
            self._sPageBody = sHtmlNavigation + '<p align="center"><i>No data to display</i></p>\n';
        return True;

    def _generatePage(self):
        """Override parent handler in order to change page title."""
        if self._sPageTitle is not None:
            self._sPageTitle = 'Test Results - ' + self._sPageTitle

        return WuiDispatcherBase._generatePage(self)

    def actionTestResultDetails(self):
        """Show test case execution result details."""
        from testmanager.webui.wuitestresult import WuiTestResult;

        self._sTemplate = 'template-details.html';
        idTestSet = self.getIntParam(TestSetData.ksParam_idTestSet);
        self._checkForUnknownParameters()

        oTestSetData          = TestSetData().initFromDbWithId(self._oDb, idTestSet);
        try:
            (oTestResultTree, _) = TestResultLogic(self._oDb).fetchResultTree(idTestSet);
        except TMTooManyRows:
            (oTestResultTree, _) = TestResultLogic(self._oDb).fetchResultTree(idTestSet, 2);
        oBuildDataEx          = BuildDataEx().initFromDbWithId(self._oDb, oTestSetData.idBuild, oTestSetData.tsCreated);
        try:    oBuildValidationKitDataEx = BuildDataEx().initFromDbWithId(self._oDb, oTestSetData.idBuildTestSuite,
                                                                           oTestSetData.tsCreated);
        except: oBuildValidationKitDataEx = None;
        oTestBoxData          = TestBoxData().initFromDbWithGenId(self._oDb, oTestSetData.idGenTestBox);
        oTestGroupData        = TestGroupData().initFromDbWithId(self._oDb,  ## @todo This bogus time wise. Bad DB design?
                                                                 oTestSetData.idTestGroup, oTestSetData.tsCreated);
        oTestCaseDataEx       = TestCaseDataEx().initFromDbWithGenId(self._oDb, oTestSetData.idGenTestCase,
                                                                     oTestSetData.tsConfig);
        oTestCaseArgsDataEx   = TestCaseArgsDataEx().initFromDbWithGenIdEx(self._oDb, oTestSetData.idGenTestCaseArgs,
                                                                           oTestSetData.tsConfig);

        oContent = WuiTestResult(oDisp = self, fnDPrint = self._oSrvGlue.dprint);
        (self._sPageTitle, self._sPageBody) = oContent.showTestCaseResultDetails(oTestResultTree,
                                                                                 oTestSetData,
                                                                                 oBuildDataEx,
                                                                                 oBuildValidationKitDataEx,
                                                                                 oTestBoxData,
                                                                                 oTestGroupData,
                                                                                 oTestCaseDataEx,
                                                                                 oTestCaseArgsDataEx);
        return True

    def actionViewLog(self):
        """
        Log viewer action.
        """
        from testmanager.webui.wuilogviewer import WuiLogViewer;
        self._sTemplate = 'template-details.html'; ## @todo create new template (background color, etc)
        idTestSet       = self.getIntParam(self.ksParamLogSetId,     iMin = 1);
        idLogFile       = self.getIntParam(self.ksParamLogFileId,    iMin = 0,                    iDefault = 0);
        cbChunk         = self.getIntParam(self.ksParamLogChunkSize, iMin = 256, iMax = 16777216, iDefault = 65536);
        iChunk          = self.getIntParam(self.ksParamLogChunkNo,   iMin = 0,
                                           iMax = config.g_kcMbMaxMainLog * 1048576 / cbChunk,    iDefault = 0);
        self._checkForUnknownParameters();

        oTestSet = TestSetData().initFromDbWithId(self._oDb, idTestSet);
        if idLogFile == 0:
            oTestFile = TestResultFileDataEx().initFakeMainLog(oTestSet);
        else:
            oTestFile = TestSetLogic(self._oDb).getFile(idTestSet, idLogFile);
        if oTestFile.sMime not in [ 'text/plain',]:
            raise WuiException('The log view does not display files of type: %s' % (oTestFile.sMime,));

        oContent = WuiLogViewer(oTestSet, oTestFile, cbChunk, iChunk, oDisp = self, fnDPrint = self._oSrvGlue.dprint);
        (self._sPageTitle, self._sPageBody) = oContent.show();
        return True;

    def actionGetFile(self):
        """
        Get file action.
        """
        idTestSet       = self.getIntParam(self.ksParamGetFileSetId,        iMin = 1);
        idFile          = self.getIntParam(self.ksParamGetFileId,           iMin = 0, iDefault = 0);
        fDownloadIt     = self.getBoolParam(self.ksParamGetFileDownloadIt,  fDefault = True);
        self._checkForUnknownParameters();

        #
        # Get the file info and open it.
        #
        oTestSet = TestSetData().initFromDbWithId(self._oDb, idTestSet);
        if idFile == 0:
            oTestFile = TestResultFileDataEx().initFakeMainLog(oTestSet);
        else:
            oTestFile = TestSetLogic(self._oDb).getFile(idTestSet, idFile);

        (oFile, oSizeOrError, _) = oTestSet.openFile(oTestFile.sFile, 'rb');
        if oFile is None:
            raise Exception(oSizeOrError);

        #
        # Send the file.
        #
        self._oSrvGlue.setHeaderField('Content-Type', oTestFile.getMimeWithEncoding());
        if fDownloadIt:
            self._oSrvGlue.setHeaderField('Content-Disposition', 'attachment; filename="TestSet-%d-%s"'
                                          % (idTestSet, oTestFile.sFile,));
        while True:
            abChunk = oFile.read(262144);
            if len(abChunk) == 0:
                break;
            self._oSrvGlue.writeRaw(abChunk);
        return self.ksDispatchRcAllDone;

    def _actionGenericReport(self, oModelType, oReportType):
        """
        Generic report action.
        oReportType is a child of WuiReportContentBase.
        oModelType is a child of ReportModelBase.
        """
        tsEffective     = self.getEffectiveDateParam();
        cPeriods        = self.getIntParam(self.ksParamReportPeriods,       iMin = 2, iMax =   99,  iDefault = 7);
        cHoursPerPeriod = self.getIntParam(self.ksParamReportPeriodInHours, iMin = 1, iMax =   168, iDefault = 24);
        sSubject        = self.getStringParam(self.ksParamReportSubject, ReportModelBase.kasSubjects,
                                              ReportModelBase.ksSubEverything);
        if sSubject == ReportModelBase.ksSubEverything:
            aidSubjects = self.getListOfIntParams(self.ksParamReportSubjectIds, aiDefaults = []);
        else:
            aidSubjects = self.getListOfIntParams(self.ksParamReportSubjectIds, iMin = 1);
            if aidSubjects is None:
                raise WuiException('Missing parameter %s' % (self.ksParamReportSubjectIds,));
        self._checkForUnknownParameters();

        dParams = \
        {
            self.ksParamEffectiveDate:          tsEffective,
            self.ksParamReportPeriods:          cPeriods,
            self.ksParamReportPeriodInHours:    cHoursPerPeriod,
            self.ksParamReportSubject:          sSubject,
            self.ksParamReportSubjectIds:       aidSubjects,
        };

        oModel   = oModelType(self._oDb, tsEffective, cPeriods, cHoursPerPeriod, sSubject, aidSubjects);
        oContent = oReportType(oModel, dParams, fSubReport = False, fnDPrint = self._oSrvGlue.dprint, oDisp = self);
        (self._sPageTitle, self._sPageBody) = oContent.show();
        sNavi = self._generateReportNavigation(tsEffective, cHoursPerPeriod, cPeriods);
        self._sPageBody = sNavi + self._sPageBody;
        return True;

    def _actionGraphWiz(self):
        """
        Graph wizard action.
        """
        from testmanager.webui.wuigraphwiz import WuiGraphWiz;
        self._sTemplate = 'template-graphwiz.html';

        tsEffective     = self.getEffectiveDateParam();
        cPeriods        = self.getIntParam(self.ksParamReportPeriods, iMin = 1, iMax = 1, iDefault = 1); # Not needed yet.
        sTmp            = self.getStringParam(self.ksParamReportPeriodInHours, sDefault = '3 weeks');
        (cHoursPerPeriod, sError) = utils.parseIntervalHours(sTmp);
        if sError is not None: raise WuiException(sError);
        asSubjectIds    = self.getListOfStrParams(self.ksParamReportSubjectIds);
        sSubject        = self.getStringParam(self.ksParamReportSubject, [ReportModelBase.ksSubEverything],
                                              ReportModelBase.ksSubEverything); # dummy
        aidTestBoxes    = self.getListOfIntParams(self.ksParamGraphWizTestBoxIds,  iMin = 1, aiDefaults = []);
        aidBuildCats    = self.getListOfIntParams(self.ksParamGraphWizBuildCatIds, iMin = 1, aiDefaults = []);
        aidTestCases    = self.getListOfIntParams(self.ksParamGraphWizTestCaseIds, iMin = 1, aiDefaults = []);
        fSepTestVars    = self.getBoolParam(self.ksParamGraphWizSepTestVars, fDefault = False);

        enmGraphImpl    = self.getStringParam(self.ksParamGraphWizImpl, asValidValues = self.kasGraphWizImplValid,
                                              sDefault = self.ksGraphWizImpl_Default);
        cx              = self.getIntParam(self.ksParamGraphWizWidth,  iMin = 128, iMax = 8192, iDefault = 1280);
        cy              = self.getIntParam(self.ksParamGraphWizHeight, iMin = 128, iMax = 8192, iDefault = int(cx * 5 / 16) );
        cDotsPerInch    = self.getIntParam(self.ksParamGraphWizDpi,    iMin =  64, iMax =  512, iDefault = 96);
        cPtFont         = self.getIntParam(self.ksParamGraphWizFontSize, iMin = 6, iMax =  32,  iDefault = 8);
        fErrorBarY      = self.getBoolParam(self.ksParamGraphWizErrorBarY, fDefault = False);
        cMaxErrorBarY   = self.getIntParam(self.ksParamGraphWizMaxErrorBarY, iMin = 8, iMax = 9999999, iDefault = 18);
        cMaxPerGraph    = self.getIntParam(self.ksParamGraphWizMaxPerGraph, iMin = 1, iMax = 24, iDefault = 8);
        fXkcdStyle      = self.getBoolParam(self.ksParamGraphWizXkcdStyle, fDefault = False);
        fTabular        = self.getBoolParam(self.ksParamGraphWizTabular, fDefault = False);
        idSrcTestSet    = self.getIntParam(self.ksParamGraphWizSrcTestSetId, iDefault = None);
        self._checkForUnknownParameters();

        dParams = \
        {
            self.ksParamEffectiveDate:          tsEffective,
            self.ksParamReportPeriods:          cPeriods,
            self.ksParamReportPeriodInHours:    cHoursPerPeriod,
            self.ksParamReportSubject:          sSubject,
            self.ksParamReportSubjectIds:       asSubjectIds,
            self.ksParamGraphWizTestBoxIds:     aidTestBoxes,
            self.ksParamGraphWizBuildCatIds:    aidBuildCats,
            self.ksParamGraphWizTestCaseIds:    aidTestCases,
            self.ksParamGraphWizSepTestVars:    fSepTestVars,

            self.ksParamGraphWizImpl:           enmGraphImpl,
            self.ksParamGraphWizWidth:          cx,
            self.ksParamGraphWizHeight:         cy,
            self.ksParamGraphWizDpi:            cDotsPerInch,
            self.ksParamGraphWizFontSize:       cPtFont,
            self.ksParamGraphWizErrorBarY:      fErrorBarY,
            self.ksParamGraphWizMaxErrorBarY:   cMaxErrorBarY,
            self.ksParamGraphWizMaxPerGraph:    cMaxPerGraph,
            self.ksParamGraphWizXkcdStyle:      fXkcdStyle,
            self.ksParamGraphWizTabular:        fTabular,
            self.ksParamGraphWizSrcTestSetId:   idSrcTestSet,
        };

        oModel   = ReportGraphModel(self._oDb, tsEffective, cPeriods, cHoursPerPeriod, sSubject, asSubjectIds,
                                    aidTestBoxes, aidBuildCats, aidTestCases, fSepTestVars);
        oContent = WuiGraphWiz(oModel, dParams, fSubReport = False, fnDPrint = self._oSrvGlue.dprint, oDisp = self);
        (self._sPageTitle, self._sPageBody) = oContent.show();
        return True;

    def _actionVcsHistoryTooltip(self):
        """
        Version control system history.
        """
        self._sTemplate = 'template-tooltip.html';
        from testmanager.webui.wuivcshistory import WuiVcsHistoryTooltip;

        iRevision   = self.getIntParam(self.ksParamVcsHistoryRevision, iMin = 0, iMax = 999999999);
        sRepository = self.getStringParam(self.ksParamVcsHistoryRepository);
        cEntries    = self.getIntParam(self.ksParamVcsHistoryEntries, iMin = 1, iMax = 1024, iDefault = 8);
        self._checkForUnknownParameters();

        aoEntries = VcsRevisionLogic(self._oDb).fetchTimeline(sRepository, iRevision, cEntries);
        oContent  = WuiVcsHistoryTooltip(aoEntries, sRepository, iRevision, cEntries,
                                         fnDPrint = self._oSrvGlue.dprint, oDisp = self);
        (self._sPageTitle, self._sPageBody) = oContent.show();
        return True;

