/* $Id: UINetworkCustomer.cpp $ */
/** @file
 * VBox Qt GUI - UINetworkCustomer class implementation.
 */

/*
 * Copyright (C) 2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifdef VBOX_WITH_PRECOMPILED_HEADERS
# include <precomp.h>
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */

/* Qt includes: */
# include <QNetworkRequest>

/* Local includes: */
# include "UINetworkCustomer.h"
# include "UINetworkManager.h"

#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */


UINetworkCustomer::UINetworkCustomer()
    : QObject(0)
    , m_fForceCall(true)
{
}

UINetworkCustomer::UINetworkCustomer(QObject *pParent, bool fForceCall)
    : QObject(pParent)
    , m_fForceCall(fForceCall)
{
}

void UINetworkCustomer::createNetworkRequest(const QNetworkRequest &request, UINetworkRequestType type, const QString &strDescription)
{
    gNetworkManager->createNetworkRequest(request, type, strDescription, this);
}

void UINetworkCustomer::createNetworkRequest(const QList<QNetworkRequest> &requests, UINetworkRequestType type, const QString &strDescription)
{
    gNetworkManager->createNetworkRequest(requests, type, strDescription, this);
}

