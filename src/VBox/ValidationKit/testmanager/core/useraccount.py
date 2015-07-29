# -*- coding: utf-8 -*-
# $Id: useraccount.py $

"""
Test Manager - User DB records management.
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


# Standard python imports.
import unittest;

# Validation Kit imports.
from testmanager            import config;
from testmanager.core.base  import ModelDataBase, ModelLogicBase, ModelDataBaseTestCase, TMExceptionBase;


class UserAccountData(ModelDataBase):
    """
    User account data
    """

    ksIdAttr    = 'uid';

    ksParam_uid                 = 'UserAccount_uid'
    ksParam_tsExpire            = 'UserAccount_tsExpire'
    ksParam_tsEffective         = 'UserAccount_tsEffective'
    ksParam_uidAuthor           = 'UserAccount_uidAuthor'
    ksParam_sLoginName          = 'UserAccount_sLoginName'
    ksParam_sUsername           = 'UserAccount_sUsername'
    ksParam_sEmail              = 'UserAccount_sEmail'
    ksParam_sFullName           = 'UserAccount_sFullName'

    kasAllowNullAttributes      = ['uid', 'tsEffective', 'tsExpire', 'uidAuthor'];


    def __init__(self):
        """Init parameters"""
        ModelDataBase.__init__(self);
        self.uid            = None;
        self.tsEffective    = None;
        self.tsExpire       = None;
        self.uidAuthor      = None;
        self.sUsername      = None;
        self.sEmail         = None;
        self.sFullName      = None;
        self.sLoginName     = None;

    def initFromDbRow(self, aoRow):
        """
        Init from database table row
        Returns self. Raises exception of the row is None.
        """
        if aoRow is None:
            raise TMExceptionBase('User not found.');

        self.uid            = aoRow[0];
        self.tsEffective    = aoRow[1];
        self.tsExpire       = aoRow[2];
        self.uidAuthor      = aoRow[3];
        self.sUsername      = aoRow[4];
        self.sEmail         = aoRow[5];
        self.sFullName      = aoRow[6];
        self.sLoginName     = aoRow[7];
        return self;

    def initFromDbWithId(self, oDb, uid, tsNow = None, sPeriodBack = None):
        """
        Initialize the object from the database.
        """
        oDb.execute(self.formatSimpleNowAndPeriodQuery(oDb,
                                                       'SELECT *\n'
                                                       'FROM   Users\n'
                                                       'WHERE  uid = %s\n'
                                                       , ( uid, ), tsNow, sPeriodBack));
        aoRow = oDb.fetchOne()
        if aoRow is None:
            raise TMExceptionBase('uid=%s not found (tsNow=%s sPeriodBack=%s)' % (uid, tsNow, sPeriodBack,));
        return self.initFromDbRow(aoRow);

    def _validateAndConvertAttribute(self, sAttr, sParam, oValue, aoNilValues, fAllowNull, oDb):
        # Custom handling of the email field.
        if sAttr == 'sEmail':
            return ModelDataBase.validateEmail(oValue, aoNilValues = aoNilValues, fAllowNull = fAllowNull);

        # Automatically lowercase the login name if we're supposed to do case
        # insensitive matching.  (The feature assumes lower case in DB.)
        if sAttr == 'sLoginName' and oValue is not None and config.g_kfLoginNameCaseInsensitive:
            oValue = oValue.lower();

        return ModelDataBase._validateAndConvertAttribute(self, sAttr, sParam, oValue, aoNilValues, fAllowNull, oDb);


class UserAccountLogic(ModelLogicBase):
    """
    SystemLog logic.
    """

    def fetchForListing(self, iStart, cMaxRows, tsNow):
        """
        Fetches user accounts.

        Returns an array (list) of UserAccountData items, empty list if none.
        Raises exception on error.
        """
        if tsNow is None:
            self._oDb.execute('SELECT   *\n'
                              'FROM     Users\n'
                              'WHERE    tsExpire = \'infinity\'::TIMESTAMP\n'
                              'ORDER BY sUsername DESC\n'
                              'LIMIT %s OFFSET %s\n'
                              , (cMaxRows, iStart,));
        else:
            self._oDb.execute('SELECT   *\n'
                              'FROM     Users\n'
                              'WHERE    tsExpire     > %s\n'
                              '     AND tsEffective <= %s\n'
                              'ORDER BY sUsername DESC\n'
                              'LIMIT %s OFFSET %s\n'
                              , (tsNow, tsNow, cMaxRows, iStart,));

        aoRows = [];
        for _ in range(self._oDb.getRowCount()):
            aoRows.append(UserAccountData().initFromDbRow(self._oDb.fetchOne()));
        return aoRows;

    def addEntry(self, oData, uidAuthor, fCommit = False):
        """
        Add user account entry to the DB.
        """
        self._oDb.callProc('UserAccountLogic_addEntry',
                           (uidAuthor, oData.sUsername, oData.sEmail, oData.sFullName, oData.sLoginName,));
        self._oDb.maybeCommit(fCommit);
        return True;

    def editEntry(self, oData, uidAuthor, fCommit = False):
        """
        Modify user account.
        """
        self._oDb.callProc('UserAccountLogic_editEntry',
                           (uidAuthor, oData.uid, oData.sUsername, oData.sEmail, oData.sFullName, oData.sLoginName,));
        self._oDb.maybeCommit(fCommit);
        return True;

    def removeEntry(self, uidAuthor, uid, fCascade = False, fCommit = False):
        """
        Delete user account
        """
        self._oDb.callProc('UserAccountLogic_delEntry', (uidAuthor, uid));
        self._oDb.maybeCommit(fCommit);
        _ = fCascade;
        return True;

    def _getByField(self, sField, sValue):
        """
        Get user account record by its field value
        """
        self._oDb.execute('SELECT   *\n'
                          'FROM     Users\n'
                          'WHERE    tsExpire = \'infinity\'::TIMESTAMP\n'
                          '  AND    ' + sField + ' = %s'
                          , (sValue,))

        aRows = self._oDb.fetchAll()
        if len(aRows) not in (0, 1):
            raise TMExceptionBase('Found more than one user account with the same credentials. Database structure is corrupted.')

        try:
            return aRows[0]
        except IndexError:
            return []

    def getById(self, idUserId):
        """
        Get user account information by ID.
        """
        return self._getByField('uid', idUserId)

    def tryFetchAccountByLoginName(self, sLoginName):
        """
        Try get user account information by login name.

        Returns UserAccountData if found, None if not.
        Raises exception on DB error.
        """
        if config.g_kfLoginNameCaseInsensitive:
            sLoginName = sLoginName.lower();

        self._oDb.execute('SELECT   *\n'
                          'FROM     Users\n'
                          'WHERE    sLoginName = %s\n'
                          '     AND tsExpire = \'infinity\'::TIMESTAMP\n'
                          , (sLoginName, ));
        if self._oDb.getRowCount() != 1:
            if self._oDb.getRowCount() != 0:
                raise self._oDb.integrityException('%u rows in Users with sLoginName="%s"'
                                                   % (self._oDb.getRowCount(), sLoginName));
            return None;
        return UserAccountData().initFromDbRow(self._oDb.fetchOne());

    def resolveChangeLogAuthors(self, aoEntries):
        """
        Given an array of ChangeLogEntry instances, set sAuthor to whatever
        uidAuthor resolves to.

        Returns aoEntries.
        Raises exception on DB error.
        """
        ahCache = dict();
        for oEntry in aoEntries:
            oEntry.sAuthor = ahCache.get(oEntry.uidAuthor, None);
            if oEntry.sAuthor is None and oEntry.uidAuthor is not None:
                try:
                    oUser = UserAccountData().initFromDbWithId(self._oDb, oEntry.uidAuthor, oEntry.tsEffective);
                except:
                    pass;
                else:
                    ahCache[oEntry.uidAuthor] = oUser.sUsername;
                    oEntry.sAuthor = oUser.sUsername;
        return aoEntries;


#
# Unit testing.
#

# pylint: disable=C0111
class UserAccountDataTestCase(ModelDataBaseTestCase):
    def setUp(self):
        self.aoSamples = [UserAccountData(),];

if __name__ == '__main__':
    unittest.main();
    # not reached.

