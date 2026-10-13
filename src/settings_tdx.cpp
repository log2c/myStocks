#include "settings_dialog.h"
#include "tdx_bridge.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QString normalizedServerAddress(const QString& input) {
    const QString text = input.trimmed();
    const QUrl url(QStringLiteral("tcp://") + text);
    if (!url.isValid() || url.host().isEmpty() || url.port() < 1 || url.port() > 65535
        || !url.userInfo().isEmpty() || !url.path().isEmpty() || url.hasQuery() || url.hasFragment()
        || text.contains(QRegularExpression(QStringLiteral("\\s")))) {
        return {};
    }
    const QString host = url.host().toLower();
    return (host.contains(':') ? QStringLiteral("[%1]").arg(host) : host)
        + ':' + QString::number(url.port());
}
}

QWidget* SettingsDialog::buildTdxTab() {
    QWidget* w = new QWidget(this);
    auto* root = new QVBoxLayout(w);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(8);

    auto* login = new QGroupBox(trText("settings.tdx.loginGroup"), w);
    auto* form = new QFormLayout(login);
    m_tdxUserIdEdit = new QLineEdit(m_cfg.tdxUserId, login);
    m_tdxUserIdEdit->setPlaceholderText(trText("settings.tdx.userIdPlaceholder"));
    m_tdxTokenEdit = new QLineEdit(m_cfg.tdxToken, login);
    m_tdxTokenEdit->setEchoMode(QLineEdit::Password);
    m_tdxTokenEdit->setPlaceholderText(trText("settings.tdx.tokenPlaceholder"));
    m_tdxRememberTokenCheck = new QCheckBox(trText("settings.tdx.rememberToken"), login);
    m_tdxRememberTokenCheck->setChecked(m_cfg.tdxRememberToken);
    auto* loginButton = new QPushButton(trText("settings.tdx.login"), login);
    loginButton->setEnabled(false);
    loginButton->setToolTip(trText("settings.tdx.hint"));
    form->addRow(trText("settings.tdx.userId"), m_tdxUserIdEdit);
    form->addRow(trText("settings.tdx.token"), m_tdxTokenEdit);
    form->addRow(QString(), m_tdxRememberTokenCheck);
    form->addRow(QString(), loginButton);
    auto* loginHint = new QLabel(trText("settings.tdx.hint"), login);
    loginHint->setWordWrap(true);
    form->addRow(loginHint);
    root->addWidget(login);

    auto* hint = new QLabel(trText("settings.tdx.serversHint"), w);
    hint->setWordWrap(true);
    root->addWidget(hint);
    m_tdxServerSelection[0] = m_cfg.tdxMainServer;
    m_tdxServerSelection[1] = m_cfg.tdxExServer;
    for (int i = 0; i < 2; ++i) {
        const bool extended = i == 1;
        auto* box = new QGroupBox(trText(extended ? "settings.tdx.exServers" : "settings.tdx.mainServers"), w);
        auto* layout = new QVBoxLayout(box);
        m_tdxAutoSelect[i] = new QCheckBox(trText("settings.tdx.autoSelect"), box);
        m_tdxAutoSelect[i]->setChecked(extended ? m_cfg.tdxExAutoSelect : m_cfg.tdxMainAutoSelect);
        layout->addWidget(m_tdxAutoSelect[i]);
        m_tdxSelectedServer[i] = new QLabel(box);
        m_tdxSelectedServer[i]->setWordWrap(true);
        layout->addWidget(m_tdxSelectedServer[i]);
        auto* table = new QTableWidget(0, 4, box);
        m_tdxServerTables[i] = table;
        table->setObjectName(extended ? QStringLiteral("tdxExServers") : QStringLiteral("tdxMainServers"));
        table->setHorizontalHeaderLabels({trText("settings.tdx.nodeName"), trText("settings.tdx.address"),
            trText("settings.tdx.latency"), trText("settings.tdx.status")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->setMinimumHeight(130);
        table->setMaximumHeight(180);
        layout->addWidget(table);
        auto* actions = new QHBoxLayout;
        auto* add = new QPushButton(trText("settings.tdx.addServer"), box);
        auto* remove = new QPushButton(trText("settings.tdx.removeServer"), box);
        auto* restore = new QPushButton(trText("settings.tdx.restorePool"), box);
        auto* select = new QPushButton(trText("settings.tdx.useSelected"), box);
        for (auto* button : {add, remove, restore, select}) {
            button->setAutoDefault(false);
            actions->addWidget(button);
        }
        layout->addLayout(actions);
        auto* testing = new QHBoxLayout;
        auto* testSelected = new QPushButton(trText("settings.tdx.testSelected"), box);
        testSelected->setAutoDefault(false);
        m_tdxTestButtons[i] = new QPushButton(trText("settings.tdx.testPool"), box);
        m_tdxTestButtons[i]->setAutoDefault(false);
        testing->addWidget(testSelected);
        testing->addWidget(m_tdxTestButtons[i]);
        testing->addStretch();
        layout->addLayout(testing);
        m_tdxServerStatus[i] = new QLabel(trText("settings.tdx.loadingServers"), box);
        m_tdxServerStatus[i]->setWordWrap(true);
        layout->addWidget(m_tdxServerStatus[i]);
        root->addWidget(box);
        populateTdxServers(extended, extended ? m_cfg.tdxExServerPool : m_cfg.tdxMainServerPool);
        connect(m_tdxAutoSelect[i], &QCheckBox::toggled, this, [this, i, extended](bool automatic) {
            if (!automatic && m_tdxServerSelection[i].isEmpty()) {
                const QStringList addresses = tdxServerAddresses(extended);
                if (!addresses.isEmpty()) m_tdxServerSelection[i] = addresses.first();
            }
            m_tdxSelectedServer[i]->setText(automatic ? trText("settings.tdx.autoSelected")
                : trText("settings.tdx.selectedServer").arg(m_tdxServerSelection[i]));
        });
        connect(add, &QPushButton::clicked, this, [this, i, extended]() {
            if (m_tdxServersLoading) return;
            bool ok = false;
            const QString input = QInputDialog::getText(this, trText("settings.tdx.addServer"),
                trText("settings.tdx.addressPrompt"), QLineEdit::Normal, QString(), &ok);
            if (!ok) return;
            const QString address = normalizedServerAddress(input);
            if (address.isEmpty()) {
                QMessageBox::warning(this, trText("settings.tab.tdx"), trText("settings.tdx.invalidAddress"));
                return;
            }
            QStringList addresses = tdxServerAddresses(extended);
            if (addresses.size() >= 64) {
                QMessageBox::warning(this, trText("settings.tab.tdx"), trText("settings.tdx.poolLimit"));
                return;
            }
            if (!addresses.contains(address)) { addresses.append(address); populateTdxServers(extended, addresses); }
            m_tdxServerTables[i]->selectRow(addresses.indexOf(address));
        });
        connect(remove, &QPushButton::clicked, this, [this, i, extended]() {
            if (m_tdxServersLoading) return;
            const int row = m_tdxServerTables[i]->currentRow();
            QStringList addresses = tdxServerAddresses(extended);
            if (row < 0 || row >= addresses.size()) return;
            if (addresses.size() == 1) {
                QMessageBox::warning(this, trText("settings.tab.tdx"), trText("settings.tdx.keepOneServer"));
                return;
            }
            if (addresses.takeAt(row) == m_tdxServerSelection[i]) {
                m_tdxServerSelection[i].clear();
                m_tdxAutoSelect[i]->setChecked(true);
            }
            populateTdxServers(extended, addresses);
        });
        connect(restore, &QPushButton::clicked, this, [this, i, extended]() {
            if (m_tdxServersLoading || m_tdxBuiltinServerPools[i].isEmpty()) return;
            m_tdxServerSelection[i].clear();
            m_tdxAutoSelect[i]->setChecked(true);
            populateTdxServers(extended, m_tdxBuiltinServerPools[i]);
        });
        connect(select, &QPushButton::clicked, this, [this, i]() {
            const int row = m_tdxServerTables[i]->currentRow();
            if (row < 0) return;
            m_tdxServerSelection[i] = m_tdxServerTables[i]->item(row, 1)->text();
            m_tdxAutoSelect[i]->setChecked(false);
            m_tdxSelectedServer[i]->setText(trText("settings.tdx.selectedServer").arg(m_tdxServerSelection[i]));
        });
        connect(testSelected, &QPushButton::clicked, this, [this, extended]() { testTdxServers(extended, true); });
        connect(m_tdxTestButtons[i], &QPushButton::clicked, this, [this, extended]() { testTdxServers(extended); });
    }
    root->addStretch();
    m_tdxServersLoading = true;
    QTimer::singleShot(0, this, [this]() { loadTdxServers(); });
    return w;
}

QStringList SettingsDialog::tdxServerAddresses(bool extended) const {
    QStringList addresses;
    const auto* table = m_tdxServerTables[extended ? 1 : 0];
    if (!table) return addresses;
    for (int row = 0; row < table->rowCount(); ++row) addresses.append(table->item(row, 1)->text());
    return addresses;
}

void SettingsDialog::populateTdxServers(bool extended, const QStringList& addresses) {
    const int i = extended ? 1 : 0;
    auto* table = m_tdxServerTables[i];
    table->setRowCount(0);
    for (const QString& address : addresses) {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(m_tdxBuiltinServerNames[i].value(address, trText("settings.tdx.customServer"))));
        table->setItem(row, 1, new QTableWidgetItem(address));
        table->setItem(row, 2, new QTableWidgetItem(QStringLiteral("--")));
        table->setItem(row, 3, new QTableWidgetItem(trText("settings.tdx.untested")));
        if (address == m_tdxServerSelection[i]) table->selectRow(row);
    }
    m_tdxSelectedServer[i]->setText(m_tdxAutoSelect[i]->isChecked() ? trText("settings.tdx.autoSelected")
        : trText("settings.tdx.selectedServer").arg(m_tdxServerSelection[i]));
}

void SettingsDialog::loadTdxServers() {
    tdx_bridge::request(this, m_cfg, {{QStringLiteral("operation"), QStringLiteral("server_list")}},
        [this](const QJsonObject& result, const QString& error) {
            m_tdxServersLoading = false;
            if (!error.isEmpty()) {
                for (int i = 0; i < 2; ++i) m_tdxServerStatus[i]->setText(trText("settings.tdx.loadFailed").arg(error));
                return;
            }
            for (int i = 0; i < 2; ++i) {
                const auto group = result.value(i == 1 ? QStringLiteral("ex") : QStringLiteral("main")).toObject();
                for (const auto& value : group.value(QStringLiteral("hosts")).toArray()) {
                    const auto host = value.toObject();
                    const QString address = host.value(QStringLiteral("address")).toString();
                    m_tdxBuiltinServerNames[i].insert(address, host.value(QStringLiteral("name")).toString());
                    m_tdxBuiltinServerPools[i].append(address);
                }
                const QStringList saved = i == 1 ? m_cfg.tdxExServerPool : m_cfg.tdxMainServerPool;
                populateTdxServers(i == 1, saved.isEmpty() ? m_tdxBuiltinServerPools[i] : saved);
                const QString current = group.value(QStringLiteral("current")).toString();
                m_tdxServerStatus[i]->setText(current.isEmpty() ? trText("settings.tdx.noConnection")
                    : trText("settings.tdx.currentServer").arg(current));
            }
        });
}

void SettingsDialog::testTdxServers(bool extended, bool selectedOnly) {
    if (m_tdxServersLoading || m_tdxProbePending) return;
    const int i = extended ? 1 : 0;
    AppConfig draft = config();
    if (selectedOnly) {
        const int row = m_tdxServerTables[i]->currentRow();
        if (row < 0) return;
        const QStringList selected{m_tdxServerTables[i]->item(row, 1)->text()};
        if (extended) draft.tdxExServerPool = selected;
        else draft.tdxMainServerPool = selected;
    }
    m_tdxProbePending = true;
    for (auto* button : m_tdxTestButtons) button->setEnabled(false);
    m_tdxServerStatus[i]->setText(trText("settings.tdx.testing"));
    tdx_bridge::request(this, draft, {{QStringLiteral("operation"), QStringLiteral("server_probe")},
        {QStringLiteral("kind"), extended ? QStringLiteral("ex") : QStringLiteral("main")}},
        [this, i](const QJsonObject& result, const QString& error) {
            m_tdxProbePending = false;
            for (auto* button : m_tdxTestButtons) button->setEnabled(true);
            if (!error.isEmpty()) {
                m_tdxServerStatus[i]->setText(trText("settings.tdx.testFailed").arg(error));
                return;
            }
            int reachable = 0;
            QString fastest;
            const auto results = result.value(QStringLiteral("results")).toArray();
            for (const auto& value : results) {
                const auto probe = value.toObject();
                const QString address = probe.value(QStringLiteral("address")).toString();
                const bool available = probe.value(QStringLiteral("reachable")).toBool();
                if (available) { ++reachable; if (fastest.isEmpty()) fastest = address; }
                auto* table = m_tdxServerTables[i];
                for (int row = 0; row < table->rowCount(); ++row) {
                    if (table->item(row, 1)->text() != address) continue;
                    table->item(row, 2)->setText(available
                        ? QStringLiteral("%1 ms").arg(probe.value(QStringLiteral("latencyMs")).toDouble(), 0, 'f', 1)
                        : QStringLiteral("--"));
                    table->item(row, 3)->setText(trText(available ? "settings.tdx.reachable" : "settings.tdx.unreachable"));
                    table->item(row, 3)->setToolTip(probe.value(QStringLiteral("error")).toString());
                }
            }
            m_tdxServerStatus[i]->setText(reachable == 0 ? trText("settings.tdx.noneReachable")
                : trText("settings.tdx.testComplete").arg(reachable).arg(results.size()).arg(fastest));
        });
}
