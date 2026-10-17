// Copyright (c) 2026 The Resatoshi developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/test/recyclesetuptests.h>

#include <common/args.h>
#include <common/settings.h>
#include <init.h>
#include <node/chainstatemanager_args.h>
#include <qt/optionsmodel.h>
#include <qt/recyclesetup.h>
#include <univalue.h>
#include <util/string.h>

#include <QApplication>
#include <QDir>
#include <QGroupBox>
#include <QRadioButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>

#include <fstream>

using node::GetRecycleDirectoryState;
using node::RecycleDirectoryState;

namespace {
void Configure(ArgsManager& args, const QTemporaryDir& dir)
{
    SetupServerArgs(args);
    args.ForceSetArg("-datadir", dir.path().toStdString());
    args.ForceSetArg("-regtest", "1");
    args.SelectConfigNetwork("regtest");
    fs::create_directories(args.GetDataDirNet());
}

void Write(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream file{path.std_path()};
    file << value;
}
} // namespace

void RecycleSetupTests::directoryStates()
{
    QTemporaryDir dir;
    const auto root{fs::PathFromString(dir.path().toStdString())};
    const auto blocks{root / "external-blocks"};
    QCOMPARE(GetRecycleDirectoryState(root), RecycleDirectoryState::NEW);
    Write(root / "chainstate" / "CURRENT", "legacy");
    QCOMPARE(GetRecycleDirectoryState(root), RecycleDirectoryState::EXISTING_UNMARKED);
    fs::remove_all(root / "chainstate");
    Write(blocks / "index" / "CURRENT", "legacy");
    // Raw blocks may be external; their directory is not the block-index DB.
    QCOMPARE(GetRecycleDirectoryState(root), RecycleDirectoryState::NEW);
    Write(root / "blocks" / "index" / "CURRENT", "legacy");
    QCOMPARE(GetRecycleDirectoryState(root), RecycleDirectoryState::EXISTING_UNMARKED);
    Write(root / "recycle-mode", "block\n");
    QCOMPARE(GetRecycleDirectoryState(root), RecycleDirectoryState::BLOCK);
    Write(root / "recycle-mode", "index\n");
    QCOMPARE(GetRecycleDirectoryState(root), RecycleDirectoryState::INDEX);
    Write(root / "recycle-mode", "broken\n");
    QCOMPARE(GetRecycleDirectoryState(root), RecycleDirectoryState::INVALID);
}

void RecycleSetupTests::modeFileContents_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<int>("expected");
    QTest::newRow("index") << QByteArray{"index\n"} << int(RecycleDirectoryState::INDEX);
    QTest::newRow("block") << QByteArray{"block\n"} << int(RecycleDirectoryState::BLOCK);
    QTest::newRow("no-final-newline") << QByteArray{"block"} << int(RecycleDirectoryState::BLOCK);
    QTest::newRow("empty") << QByteArray{} << int(RecycleDirectoryState::INVALID);
    QTest::newRow("blank-line") << QByteArray{"\n"} << int(RecycleDirectoryState::INVALID);
    QTest::newRow("unknown") << QByteArray{"unknown\n"} << int(RecycleDirectoryState::INVALID);
    QTest::newRow("leading-space") << QByteArray{" index\n"} << int(RecycleDirectoryState::INVALID);
    QTest::newRow("trailing-space") << QByteArray{"block \n"} << int(RecycleDirectoryState::INVALID);
    // Preserve the existing first-line-only interpretation.
    QTest::newRow("additional-line") << QByteArray{"index\nblock\n"} << int(RecycleDirectoryState::INDEX);
}

void RecycleSetupTests::modeFileContents()
{
    QFETCH(QByteArray, contents);
    QFETCH(int, expected);
    QTemporaryDir dir;
    const auto root{fs::PathFromString(dir.path().toStdString())};
    Write(root / "chainstate" / "CURRENT", "legacy");
    Write(root / "recycle-mode", contents.toStdString());
    QCOMPARE(int(GetRecycleDirectoryState(root)), expected);
    // A recorded mode, including invalid data, takes precedence over legacy DBs.
    fs::remove_all(root / "chainstate");
    QCOMPARE(int(GetRecycleDirectoryState(root)), expected);
}

void RecycleSetupTests::koreanLabels()
{
    QTranslator translator;
    QVERIFY(translator.load(":/translations/ko"));
    QApplication::installTranslator(&translator);
    RecycleSetupDialog dialog{"index", true, 0, true};
    QCOMPARE(dialog.windowTitle(), QString::fromUtf8("Resatoshi 노드 설정"));
    QCOMPARE(dialog.findChild<QRadioButton*>("indexMode")->text(), QString::fromUtf8("인덱스"));
    QCOMPARE(dialog.findChild<QRadioButton*>("blockMode")->text(), QString::fromUtf8("원본 블록"));
    const auto output{qEnvironmentVariable("RESATOSHI_GUI_SETUP_PREVIEW")};
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        dialog.show();
        QApplication::processEvents();
        QVERIFY(dialog.grab().save(output + "/index-ko.png"));
        dialog.findChild<QRadioButton*>("blockMode")->click();
        QApplication::processEvents();
        QVERIFY(dialog.grab().save(output + "/block-full-ko.png"));
        dialog.findChild<QRadioButton*>("prunedHistory")->click();
        QApplication::processEvents();
        QVERIFY(dialog.grab().save(output + "/block-prune-ko.png"));
    }
    QApplication::removeTranslator(&translator);
}

void RecycleSetupTests::translatedLabels_data()
{
    QTest::addColumn<QString>("locale");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("block_label");
    for (const auto* locale : {"zh", "zh_CN", "zh-Hans"}) {
        QTest::newRow(locale) << QString{locale} << QString::fromUtf8("设置您的 Resatoshi 节点") << QString::fromUtf8("原始区块");
    }
    for (const auto* locale : {"zh_TW", "zh-Hant", "zh_HK"}) {
        QTest::newRow(locale) << QString{locale} << QString::fromUtf8("設定您的 Resatoshi 節點") << QString::fromUtf8("原始區塊");
    }
    QTest::newRow("ja") << QString{"ja"} << QString::fromUtf8("Resatoshi ノードの設定") << QString::fromUtf8("元のブロック");
    QTest::newRow("th") << QString{"th"} << QString::fromUtf8("ตั้งค่าโหนด Resatoshi ของคุณ") << QString::fromUtf8("บล็อกต้นฉบับ");
    QTest::newRow("fr") << QString{"fr"} << QString::fromUtf8("Configurer votre nœud Resatoshi") << QString::fromUtf8("Blocs d’origine");
    QTest::newRow("de") << QString{"de"} << QString::fromUtf8("Resatoshi-Knoten einrichten") << QString::fromUtf8("Originalblöcke");
    QTest::newRow("es") << QString{"es"} << QString::fromUtf8("Configurar su nodo Resatoshi") << QString::fromUtf8("Bloques originales");
}

void RecycleSetupTests::translatedLabels()
{
    QFETCH(QString, locale);
    QFETCH(QString, title);
    QFETCH(QString, block_label);
    QTranslator translator;
    QVERIFY(translator.load(":/translations/" + locale));
    QApplication::installTranslator(&translator);
    RecycleSetupDialog dialog{"block", true, 0, true};
    // Remove even if an assertion fails, so the next test starts in English.
    QApplication::removeTranslator(&translator);
    QCOMPARE(dialog.windowTitle(), title);
    QCOMPARE(dialog.findChild<QRadioButton*>("blockMode")->text(), block_label);
    for (const auto* source : {"Cannot inspect the data folder: %1", "Cannot save node preferences: %1"}) {
        const auto translated{translator.translate("QObject", source)};
        QVERIFY(!translated.isEmpty());
        QVERIFY(translated.contains("%1"));
    }
}

void RecycleSetupTests::firstRunChoices()
{
    QTemporaryDir dir;
    ArgsManager args;
    Configure(args, dir);
    std::optional<RecycleSetupChoices> selected;
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<RecycleSetupDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        auto* block = dialog->findChild<QRadioButton*>("blockMode");
        QVERIFY(block->isEnabled());
        QVERIFY(!dialog->findChild<QGroupBox*>("blockStorage")->isVisible());
        block->click();
        QVERIFY(dialog->findChild<QGroupBox*>("blockStorage")->isVisible());
        auto* prune = dialog->findChild<QRadioButton*>("prunedHistory");
        QVERIFY(prune->isEnabled());
        prune->click();
        auto* target = dialog->findChild<QSpinBox*>("pruneTargetGB");
        QVERIFY(target->isEnabled());
        target->setValue(9);
        dialog->accept();
    });
    QVERIFY(ShowRecycleSetupIfNeeded(args, true, selected));
    QVERIFY(selected);
    QCOMPARE(selected->mode, std::string{"block"});
    QCOMPARE(selected->prune_mib, PruneGBtoMiB(9));
    QVERIFY(selected->set_prune);
    QCOMPARE(args.GetArg("-recyclemode", ""), std::string{"block"});
    QCOMPARE(args.GetIntArg("-prune", -1), PruneGBtoMiB(9));
    QVERIFY(!fs::exists(args.GetDataDirNet() / "recycle-mode"));
    QVERIFY(!fs::exists(args.GetDataDirNet() / "settings.json"));
    std::vector<std::string> errors;
    QVERIFY(SaveRecycleSetup(args, *selected, errors));
    QVERIFY(errors.empty());
    std::map<std::string, common::SettingsValue> saved;
    QVERIFY(common::ReadSettings(args.GetDataDirNet() / "settings.json", saved, errors));
    QCOMPARE(saved.at("recyclemode").get_str(), std::string{"block"});
    QCOMPARE(saved.at("prune").get_str(), util::ToString(PruneGBtoMiB(9)));

    RecycleSetupDialog full{"index", true, 0, true};
    QCOMPARE(full.windowTitle(), QString{"Set up your Resatoshi node"});
    QCOMPARE(full.findChild<QRadioButton*>("indexMode")->text(), QString{"Index"});
    full.findChild<QRadioButton*>("blockMode")->click();
    QCOMPARE(full.choices().prune_mib, int64_t{0});
    QVERIFY(full.choices().set_prune);
    full.findChild<QRadioButton*>("indexMode")->click();
    QVERIFY(!full.choices().set_prune);
}

void RecycleSetupTests::cancelDoesNotSave()
{
    QTemporaryDir dir;
    ArgsManager args;
    Configure(args, dir);
    std::optional<RecycleSetupChoices> selected;
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<RecycleSetupDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        dialog->findChild<QRadioButton*>("blockMode")->click();
        dialog->reject();
    });
    QVERIFY(!ShowRecycleSetupIfNeeded(args, true, selected));
    QVERIFY(!selected);
    QVERIFY(!args.IsArgSet("-recyclemode"));
    QVERIFY(!args.IsArgSet("-prune"));
    QVERIFY(!fs::exists(args.GetDataDirNet() / "settings.json"));
    QVERIFY(!fs::exists(args.GetDataDirNet() / "recycle-mode"));
}

void RecycleSetupTests::existingFolders()
{
    QTemporaryDir dir;
    ArgsManager args;
    Configure(args, dir);
    std::optional<RecycleSetupChoices> selected;
    Write(args.GetDataDirNet() / "recycle-mode", "block\n");
    // Saved GUI preferences cannot switch an existing folder's expiry source.
    args.LockSettings([](common::Settings& settings) { settings.rw_settings["recyclemode"] = "index"; });
    QVERIFY(ShowRecycleSetupIfNeeded(args, true, selected));
    QVERIFY(!selected);
    QCOMPARE(args.GetArg("-recyclemode", ""), std::string{"block"});
    args.ForceSetArg("-recyclemode", "index");
    QVERIFY(ShowRecycleSetupIfNeeded(args, true, selected));
    QCOMPARE(args.GetArg("-recyclemode", ""), std::string{"index"}); // core rejects explicit mismatch

    ArgsManager legacy;
    QTemporaryDir other;
    Configure(legacy, other);
    Write(legacy.GetDataDirNet() / "chainstate" / "CURRENT", "legacy");
    QVERIFY(ShowRecycleSetupIfNeeded(legacy, true, selected));
    QVERIFY(!selected);
    QCOMPARE(legacy.GetArg("-recyclemode", ""), std::string{});
}

void RecycleSetupTests::explicitConfiguration()
{
    QTemporaryDir dir;
    ArgsManager args;
    Configure(args, dir);
    // Read-only network configuration has the same precedence as CLI choices.
    args.LockSettings([](common::Settings& settings) {
        settings.ro_config["regtest"]["recyclemode"] = {"block"};
        settings.ro_config["regtest"]["prune"] = {"1"};
    });
    QVERIFY(HasRecycleStartupSetting(args, "recyclemode"));
    QVERIFY(HasRecycleStartupSetting(args, "prune"));
    std::optional<RecycleSetupChoices> selected;
    QVERIFY(ShowRecycleSetupIfNeeded(args, true, selected));
    QVERIFY(!selected);
    QCOMPARE(args.GetIntArg("-prune", -1), int64_t{1});
    RecycleSetupDialog configured{"block", false, 1, false};
    QVERIFY(!configured.findChild<QRadioButton*>("indexMode")->isEnabled());
    QVERIFY(!configured.findChild<QRadioButton*>("fullHistory")->isEnabled());
    QCOMPARE(configured.choices().prune_mib, int64_t{1});
    QVERIFY(!configured.choices().set_mode);
    QVERIFY(!configured.choices().set_prune);

    QTemporaryDir fresh;
    ArgsManager block_only;
    Configure(block_only, fresh);
    block_only.ForceSetArg("-recyclemode", "block");
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<RecycleSetupDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QVERIFY(!dialog->findChild<QRadioButton*>("indexMode")->isEnabled());
        QVERIFY(dialog->findChild<QRadioButton*>("prunedHistory")->isEnabled());
        dialog->findChild<QRadioButton*>("fullHistory")->click();
        dialog->accept();
    });
    QVERIFY(ShowRecycleSetupIfNeeded(block_only, true, selected));
    QVERIFY(selected);
    QVERIFY(!selected->set_mode);
    QVERIFY(selected->set_prune);
    QCOMPARE(block_only.GetArg("-recyclemode", ""), std::string{"block"});
    QCOMPARE(block_only.GetIntArg("-prune", -1), int64_t{0});
}

void RecycleSetupTests::saveFailure()
{
    QTemporaryDir dir;
    ArgsManager args;
    Configure(args, dir);
    args.ForceSetArg("-settings", "blocked");
    fs::create_directories(args.GetDataDirNet() / "blocked");
    const RecycleSetupChoices choice{"block", 0, true, true};
    std::vector<std::string> errors;
    QVERIFY(!SaveRecycleSetup(args, choice, errors));
    QVERIFY(!errors.empty());
    QVERIFY(!fs::exists(args.GetDataDirNet() / "recycle-mode"));
}

void RecycleSetupTests::disabledSettings()
{
    QTemporaryDir dir;
    ArgsManager args;
    Configure(args, dir);
    // Use the actual negated setting representation used by -nosettings.
    args.LockSettings([](common::Settings& settings) { settings.forced_settings["settings"] = false; });
    QVERIFY(!args.GetSettingsPath());
    std::optional<RecycleSetupChoices> selected;
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<RecycleSetupDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        dialog->findChild<QRadioButton*>("blockMode")->click();
        QVERIFY(!dialog->findChild<QRadioButton*>("prunedHistory")->isEnabled());
        dialog->accept();
    });
    QVERIFY(ShowRecycleSetupIfNeeded(args, true, selected));
    QVERIFY(selected);
    std::vector<std::string> errors;
    QVERIFY(SaveRecycleSetup(args, *selected, errors));
    QVERIFY(!fs::exists(args.GetDataDirNet() / "settings.json"));
    QVERIFY(!selected->set_prune);
    QVERIFY(ShowRecycleSetupIfNeeded(args, false, selected));
    QVERIFY(!selected);
}
