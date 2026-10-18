// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_TEST_RECYCLESETUPTESTS_H
#define BITCOIN_QT_TEST_RECYCLESETUPTESTS_H

#include <QObject>

class RecycleSetupTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void directoryStates();
    void settingsMode();
    void koreanLabels();
    void translatedLabels_data();
    void translatedLabels();
    void firstRunChoices();
    void cancelDoesNotSave();
    void existingFolders();
    void explicitConfiguration();
    void disabledSettings();
    void saveFailure();
};

#endif // BITCOIN_QT_TEST_RECYCLESETUPTESTS_H
