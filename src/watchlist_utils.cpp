#include "watchlist_utils.h"

#include <QSet>

namespace {

const QSet<QString>& predefinedAshareIndexAliases() {
    static const QSet<QString> aliases {
        QStringLiteral("sh000001"),
        QStringLiteral("sz399001"),
        QStringLiteral("sh000300"),
        QStringLiteral("sz399300"),
        QStringLiteral("sh000016"),
        QStringLiteral("sh000905"),
        QStringLiteral("sh000852"),
        QStringLiteral("sz399006"),
        QStringLiteral("sz399673"),
        QStringLiteral("sh000688"),
        QStringLiteral("sh931643"),
        QStringLiteral("sz931643"),
        QStringLiteral("sh932133"),
        QStringLiteral("sz399431"),
        QStringLiteral("sz399975"),
        QStringLiteral("sh000808"),
        QStringLiteral("sh000932"),
        QStringLiteral("sz399808"),
        QStringLiteral("sh980017"),
        QStringLiteral("sz980017"),
        QStringLiteral("399001"),
        QStringLiteral("000300"),
        QStringLiteral("399300"),
        QStringLiteral("000016"),
        QStringLiteral("000905"),
        QStringLiteral("000852"),
        QStringLiteral("399006"),
        QStringLiteral("399673"),
        QStringLiteral("000688"),
        QStringLiteral("931643"),
        QStringLiteral("932133"),
        QStringLiteral("399431"),
        QStringLiteral("399975"),
        QStringLiteral("000808"),
        QStringLiteral("000932"),
        QStringLiteral("399808"),
        QStringLiteral("980017"),
    };

    return aliases;
}

const QSet<QString>& predefinedIndexAliases() {
    static const QSet<QString> aliases = [] {
        QSet<QString> values = predefinedAshareIndexAliases();
        values.insert(QStringLiteral("hsi"));
        values.insert(QStringLiteral("hstech"));
        values.insert(QStringLiteral("100.hsi"));
        values.insert(QStringLiteral("124.hsi"));
        values.insert(QStringLiteral("100.hstech"));
        values.insert(QStringLiteral("124.hstech"));
        return values;
    }();

    return aliases;
}

} // namespace

namespace watchlist_utils {

QString watchCodeKey(const QString& code) {
    return code.trimmed().toLower();
}

bool isDigitsOnly(const QString& text) {
    if (text.isEmpty()) {
        return false;
    }

    for (const QChar ch : text) {
        if (!ch.isDigit()) {
            return false;
        }
    }

    return true;
}

QVector<int> normalizedColumnOrder(const QVector<int>& order) {
    QVector<int> out;
    out.reserve(ColCount);

    QSet<int> seen;
    for (int logical : order) {
        if (logical < 0 || logical >= ColCount || seen.contains(logical)) {
            continue;
        }
        out.push_back(logical);
        seen.insert(logical);
    }

    for (int logical = 0; logical < ColCount; ++logical) {
        if (seen.contains(logical)) {
            continue;
        }
        out.push_back(logical);
        seen.insert(logical);
    }

    return out;
}

QString normalizeSectorCode(const QString& rawCode) {
    QString code = rawCode.trimmed();
    if (code.isEmpty()) {
        return {};
    }

    if (code.startsWith(QStringLiteral("90."), Qt::CaseInsensitive)) {
        code = code.mid(3);
    }

    if (code.startsWith(QStringLiteral("bk"), Qt::CaseInsensitive)) {
        return code.toUpper();
    }

    return {};
}

QString normalizeFutureCode(const QString& rawCode) {
    const QString raw = rawCode.trimmed();
    if (raw.isEmpty()) {
        return {};
    }

    const int dot = raw.indexOf(QLatin1Char('.'));
    if (dot <= 0 || dot >= raw.size() - 1) {
        return {};
    }

    const QString market = raw.left(dot).trimmed();
    const QString symbol = raw.mid(dot + 1).trimmed().toUpper();
    if (!isDigitsOnly(market) || symbol.isEmpty()) {
        return {};
    }

    if (market == QStringLiteral("0")
        || market == QStringLiteral("1")
        || market == QStringLiteral("90")
        || market == QStringLiteral("100")
        || market == QStringLiteral("116")
        || market == QStringLiteral("124")
        || market == QStringLiteral("128")) {
        return {};
    }

    bool hasLetter = false;
    for (const QChar ch : symbol) {
        if (ch.isLetter()) {
            hasLetter = true;
            break;
        }
    }
    if (!hasLetter) {
        return {};
    }

    return market + QStringLiteral(".") + symbol;
}

QString normalizeHongKongIndexCode(const QString& rawCode) {
    const QString code = watchCodeKey(rawCode);
    if (code.isEmpty()) {
        return {};
    }

    if (code == QStringLiteral("hsi")
        || code == QStringLiteral("100.hsi")
        || code == QStringLiteral("124.hsi")) {
        return QStringLiteral("100.HSI");
    }

    if (code == QStringLiteral("hstech")
        || code == QStringLiteral("100.hstech")
        || code == QStringLiteral("124.hstech")) {
        return QStringLiteral("124.HSTECH");
    }

    return {};
}

bool isPredefinedAshareIndexCode(const QString& rawCode) {
    const QString code = watchCodeKey(rawCode);
    if (code.isEmpty()) {
        return false;
    }
    if (code.startsWith(QStringLiteral("bk")) || code.startsWith(QStringLiteral("90."))) {
        return false;
    }
    return predefinedAshareIndexAliases().contains(code);
}

bool isPredefinedIndexCode(const QString& rawCode) {
    const QString code = watchCodeKey(rawCode);
    if (code.isEmpty()) {
        return false;
    }
    if (code.startsWith(QStringLiteral("bk")) || code.startsWith(QStringLiteral("90."))) {
        return false;
    }
    return predefinedIndexAliases().contains(code);
}

bool isHongKongCode(const QString& rawCode) {
    const QString code = watchCodeKey(rawCode);
    if (code.isEmpty()) {
        return false;
    }

    if (code.startsWith(QStringLiteral("hk"))
        || code.startsWith(QStringLiteral("116."))
        || code.startsWith(QStringLiteral("128."))
        || !normalizeHongKongIndexCode(code).isEmpty()) {
        return true;
    }

    return code.size() == 5 && isDigitsOnly(code);
}

QVector<StockItem> defaultWatchStocks() {
    return {
        {QStringLiteral("sh600519"), QStringLiteral("Kweichow Moutai")},
        {QStringLiteral("sz000001"), QStringLiteral("Ping An Bank")},
        {QStringLiteral("sz300750"), QStringLiteral("CATL")}
    };
}

QString defaultDataYamlTemplate() {
    QString content;
    content += QStringLiteral("ver: 1\n\n");
    content += QStringLiteral("# stocks\n");
    content += QStringLiteral("stocks:\n");

    for (const StockItem& item : defaultWatchStocks()) {
        content += QStringLiteral("  - code: ") + item.code + QStringLiteral("\n");
        content += QStringLiteral("    name: ") + item.name + QStringLiteral("\n");
    }

    return content;
}

} // namespace watchlist_utils
