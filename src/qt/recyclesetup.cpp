// Copyright (c) 2026 The ReSatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/recyclesetup.h>

#include <common/args.h>
#include <common/settings.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <univalue.h>
#include <validation.h>

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>
#include <limits>

RecycleSetupDialog::RecycleSetupDialog(const std::string& mode, bool mode_editable,
                                       int64_t prune_mib, bool prune_editable, QWidget* parent)
    : QDialog(parent, GUIUtil::dialog_flags),
      m_mode_editable{mode_editable}, m_prune_editable{prune_editable},
      m_configured_prune{prune_mib}
{
    setObjectName("recycleSetupDialog");
    setWindowTitle(tr("Set up your ReSatoshi node"));
    setMinimumWidth(540);
    auto* layout = new QVBoxLayout(this);
    auto add_text = [](QVBoxLayout* target, const QString& text) {
        auto* label = new QLabel(text);
        label->setWordWrap(true);
        target->addWidget(label);
    };
    add_text(layout, tr("Choose how this node finds coins that have reached their expiry height. Both methods follow the same network rules and work with the wallet."));

    auto* modes = new QGroupBox(tr("Expiry processing"), this);
    auto* mode_layout = new QVBoxLayout(modes);
    m_index = new QRadioButton(tr("Index"), modes);
    m_index->setObjectName("indexMode");
    mode_layout->addWidget(m_index);
    add_text(mode_layout, tr("Uses a list of unspent coins. Automatic checks look for inconsistencies."));
    m_block = new QRadioButton(tr("Original blocks"), modes);
    m_block->setObjectName("blockMode");
    mode_layout->addWidget(m_block);
    add_text(mode_layout, tr("Reads the block that created the expiring coins and checks which coins remain unspent. Does not maintain an expiry index."));
    m_index->setChecked(mode != "block");
    m_block->setChecked(mode == "block");
    m_index->setEnabled(mode_editable);
    m_block->setEnabled(mode_editable);
    if (!mode_editable) add_text(mode_layout, tr("The expiry method is supplied by your startup configuration."));
    layout->addWidget(modes);
    add_text(layout, tr("This choice is fixed for this data folder. Changing the method later requires a new folder and synchronization. Back up your wallets before changing folders."));

    m_storage = new QGroupBox(tr("Block history"), this);
    m_storage->setObjectName("blockStorage");
    auto* storage_layout = new QVBoxLayout(m_storage);
    m_full = new QRadioButton(tr("Keep the complete history"), m_storage);
    m_full->setObjectName("fullHistory");
    m_prune = new QRadioButton(tr("Prune old blocks after expiry processing"), m_storage);
    m_prune->setObjectName("prunedHistory");
    storage_layout->addWidget(m_full);
    storage_layout->addWidget(m_prune);
    auto* target_layout = new QHBoxLayout;
    target_layout->addWidget(new QLabel(tr("Storage target (GB):"), m_storage));
    m_prune_gb = new QSpinBox(m_storage);
    m_prune_gb->setObjectName("pruneTargetGB");
    m_prune_gb->setRange(std::max(1, int((MIN_DISK_SPACE_FOR_BLOCK_FILES + 999'999'999) / 1'000'000'000)), std::numeric_limits<int>::max());
    const int target_gb{prune_mib > 1
                            ? (prune_mib >= PruneGBtoMiB(std::numeric_limits<int>::max())
                                   ? std::numeric_limits<int>::max() : PruneMiBtoGB(prune_mib))
                            : DEFAULT_PRUNE_TARGET_GB};
    m_prune_gb->setValue(target_gb);
    target_layout->addWidget(m_prune_gb);
    target_layout->addStretch();
    storage_layout->addLayout(target_layout);
    m_full->setChecked(prune_mib == 0);
    m_prune->setChecked(prune_mib != 0);
    m_full->setEnabled(prune_editable);
    m_prune->setEnabled(prune_editable);
    m_prune_gb->setEnabled(prune_editable && m_prune->isChecked());
    connect(m_prune, &QRadioButton::toggled, this, [this](bool checked) {
        m_prune_gb->setEnabled(m_prune_editable && checked);
    });
    add_text(storage_layout, tr("Pruning keeps all blocks still needed for expiry and a buffer for chain reorganizations. Actual disk use can exceed the target. Keeping the complete history supports rebuilding locally if data needs repair."));
    if (!prune_editable) add_text(storage_layout, tr("Storage is fixed by your startup configuration. To save a different storage preference here, enable settings and leave pruning unspecified."));
    layout->addWidget(m_storage);
    m_storage->setVisible(m_block->isChecked());
    connect(m_block, &QRadioButton::toggled, this, [this](bool checked) {
        m_storage->setVisible(checked);
        adjustSize();
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName("setupButtons");
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Start node"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

RecycleSetupChoices RecycleSetupDialog::choices() const
{
    const bool block{m_block->isChecked()};
    return {block ? "block" : "index",
            m_prune_editable ? (m_prune->isChecked() ? PruneGBtoMiB(m_prune_gb->value()) : 0) : m_configured_prune,
            m_mode_editable, block && m_prune_editable};
}

RecycleDirectoryState GetRecycleDirectoryState(const fs::path& datadir, const fs::path& blocksdir)
{
    const auto marker{datadir / "recycle-mode"};
    if (fs::exists(marker)) {
        std::ifstream stream{marker.std_path()};
        std::string mode;
        std::getline(stream, mode);
        if (stream && mode == "index") return RecycleDirectoryState::INDEX;
        if (stream && mode == "block") return RecycleDirectoryState::BLOCK;
        return RecycleDirectoryState::INVALID;
    }
    if (fs::exists(datadir / "chainstate" / "CURRENT") || fs::exists(blocksdir / "index" / "CURRENT")) {
        return RecycleDirectoryState::INDEX;
    }
    return RecycleDirectoryState::NEW;
}

bool HasRecycleStartupSetting(ArgsManager& args, const std::string& name)
{
    bool supplied{false};
    const auto network{args.GetChainTypeString()};
    args.LockSettings([&](const common::Settings& settings) {
        auto external{settings};
        external.rw_settings.clear();
        supplied = !common::GetSetting(external, network, name, /*ignore_default_section_config=*/false,
                                       /*ignore_nonpersistent=*/false, /*get_chain_type=*/false).isNull();
    });
    return supplied;
}

bool ShowRecycleSetupIfNeeded(ArgsManager& args, bool recycling_enabled,
                             std::optional<RecycleSetupChoices>& selected)
{
    selected.reset();
    if (!recycling_enabled) return true;
    const auto state{GetRecycleDirectoryState(args.GetDataDirNet(), args.GetBlocksDirPath())};
    const bool configured_mode{HasRecycleStartupSetting(args, "recyclemode")};
    if (state != RecycleDirectoryState::NEW) {
        // The folder remains authoritative after GUI settings are reset. Explicit
        // command-line/config mismatches are left for the core to reject.
        if (!configured_mode && state != RecycleDirectoryState::INVALID) {
            args.ForceSetArg("-recyclemode", state == RecycleDirectoryState::BLOCK ? "block" : "index");
        }
        return true;
    }
    const std::string mode{args.GetArg("-recyclemode", "index")};
    const int64_t prune{args.GetIntArg("-prune", 0)};
    const bool configured_prune{HasRecycleStartupSetting(args, "prune")};
    // Let core parameter validation report invalid explicit arguments.
    if ((mode != "index" && mode != "block") || prune < 0) return true;
    if (configured_mode && (mode == "index" || configured_prune || !args.GetSettingsPath())) return true;
    RecycleSetupDialog dialog{mode, !configured_mode, prune, !configured_prune && args.GetSettingsPath()};
    if (dialog.exec() != QDialog::Accepted) return false;
    selected = dialog.choices();
    if (selected->set_mode) args.ForceSetArg("-recyclemode", selected->mode);
    if (selected->set_prune) args.ForceSetArg("-prune", std::to_string(selected->prune_mib));
    return true;
}

bool SaveRecycleSetup(ArgsManager& args, const RecycleSetupChoices& selected,
                      std::vector<std::string>& errors)
{
    if (!args.GetSettingsPath()) return true;
    args.LockSettings([&](common::Settings& settings) {
        if (selected.set_mode) settings.rw_settings["recyclemode"] = selected.mode;
        if (selected.set_prune) settings.rw_settings["prune"] = std::to_string(selected.prune_mib);
    });
    return args.WriteSettingsFile(&errors);
}
