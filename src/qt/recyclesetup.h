// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_RECYCLESETUP_H
#define BITCOIN_QT_RECYCLESETUP_H

#include <QDialog>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class ArgsManager;
class QGroupBox;
class QRadioButton;
class QSpinBox;

struct RecycleSetupChoices {
    std::string mode;
    int64_t prune_mib{0};
    bool set_mode{true};
    bool set_prune{false};
};

/** First-run preferences, before starting a node in a fresh data directory. */
class RecycleSetupDialog : public QDialog
{
    Q_OBJECT

public:
    RecycleSetupDialog(const std::string& mode, bool mode_editable,
                       int64_t prune_mib, bool prune_editable, QWidget* parent = nullptr);
    RecycleSetupChoices choices() const;

private:
    QRadioButton* m_index;
    QRadioButton* m_block;
    QGroupBox* m_storage;
    QRadioButton* m_full;
    QRadioButton* m_prune;
    QSpinBox* m_prune_gb;
    const bool m_mode_editable;
    const bool m_prune_editable;
    const int64_t m_configured_prune;
};

bool HasRecycleStartupSetting(ArgsManager& args, const std::string& name);
/** Called after InitConfig. Cancel leaves settings and mode metadata untouched. */
bool ShowRecycleSetupIfNeeded(ArgsManager& args, bool recycling_enabled,
                             std::optional<RecycleSetupChoices>& selected);
/** Persist accepted preferences after the data directory has been locked. */
bool SaveRecycleSetup(ArgsManager& args, const RecycleSetupChoices& selected,
                      std::vector<std::string>& errors);

#endif // BITCOIN_QT_RECYCLESETUP_H
