#include "watchlist_utils.h"

#include <QSet>
#include <QRegularExpression>

namespace {

const QSet<QString>& predefinedAshareIndexAliases() {
    static const QSet<QString> aliases {
        QStringLiteral("1.000001"),
        QStringLiteral("sh000001"),
        QStringLiteral("0.399001"),
        QStringLiteral("sz399001"),
        QStringLiteral("399001"),
        QStringLiteral("0.399006"),
        QStringLiteral("399006"),
        QStringLiteral("sz399006"),
        QStringLiteral("1.000688"),
        QStringLiteral("sh000688"),
        QStringLiteral("1.000300"),
        QStringLiteral("0.399300"),
        QStringLiteral("sh000300"),
        QStringLiteral("sz399300"),
        QStringLiteral("000300"),
        QStringLiteral("399300"),
        QStringLiteral("1.000905"),
        QStringLiteral("sh000905"),
        QStringLiteral("000905"),
    };

    return aliases;
}

const QSet<QString>& predefinedIndexAliases() {
    static const QSet<QString> aliases = [] {
        QSet<QString> values = predefinedAshareIndexAliases();
        values.insert(QStringLiteral("hsi"));
        values.insert(QStringLiteral("hstech"));
        values.insert(QStringLiteral("xin9"));
        values.insert(QStringLiteral("100.hsi"));
        values.insert(QStringLiteral("124.hsi"));
        values.insert(QStringLiteral("100.hstech"));
        values.insert(QStringLiteral("124.hstech"));
        values.insert(QStringLiteral("100.xin9"));
        return values;
    }();

    return aliases;
}

QString digitsOnly(const QString& text) {
    QString digits;
    digits.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.isDigit()) {
            digits.append(ch);
        }
    }
    return digits;
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

bool isTdxCode(const QString& code) {
    static const QRegularExpression pattern(QStringLiteral("^tdx:(main|ex):([0-9]{1,3}):([A-Za-z0-9_.-]{1,22})$"), QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(code.trimmed());
    return match.hasMatch() && match.captured(2).toInt() <= 255;
}
bool isTdxCompatibleCode(const QString& rawCode) {
    const QString code = normalizeApiWatchCode(rawCode);
    if (isTdxCode(code)) return true;
    const QString market = code.section(QLatin1Char('.'), 0, 0);
    const QString symbol = code.section(QLatin1Char('.'), 1);
    if (symbol.isEmpty()) return false;
    if (market == QLatin1String("0") || market == QLatin1String("1") || market == QLatin1String("2")) {
        return symbol.size() == 6 && isDigitsOnly(symbol);
    }
    if (market == QLatin1String("116")) return symbol.size() == 5 && isDigitsOnly(symbol);
    if (market == QLatin1String("100") || market == QLatin1String("124")) {
        return symbol == QLatin1String("HSI") || symbol == QLatin1String("HSCEI") || symbol == QLatin1String("HSTECH");
    }
    if (market == QLatin1String("133")) return symbol == QLatin1String("USDCNH");
    return (market == QLatin1String("113") || market == QLatin1String("114")
        || market == QLatin1String("115") || market == QLatin1String("8"))
        && !normalizeFutureCode(code).isEmpty();
}

bool isTdxExtendedCode(const QString& code) {
    return isTdxCode(code) && code.trimmed().startsWith(QStringLiteral("tdx:ex:"), Qt::CaseInsensitive);
}
QString tdxSymbol(const QString& code) {
    return isTdxCode(code) ? code.trimmed().section(QLatin1Char(':'), 3).toUpper() : QString();
}
QString normalizeApiWatchCode(const QString& rawCode) {
    const QString raw = rawCode.trimmed();
    if (raw.isEmpty()) {
        return {};
    }

    if (isTdxCode(raw)) {
        const auto parts = raw.split(QLatin1Char(':'));
        return QStringLiteral("tdx:%1:%2:%3").arg(parts[1].toLower(), QString::number(parts[2].toInt()), parts[3].toUpper());
    }
    const QString key = watchCodeKey(raw);
    if (key == QStringLiteral("xin9") || key == QStringLiteral("100.xin9")) {
        return QStringLiteral("100.XIN9");
    }

    const QString normalizedHongKongIndex = normalizeHongKongIndexCode(raw);
    if (!normalizedHongKongIndex.isEmpty()) {
        return normalizedHongKongIndex;
    }

    const QString sectorCode = normalizeSectorCode(raw);
    if (!sectorCode.isEmpty()) {
        return QStringLiteral("90.") + sectorCode;
    }

    const QString futureCode = normalizeFutureCode(raw);
    if (!futureCode.isEmpty()) {
        return futureCode;
    }

    const int dot = raw.indexOf(QLatin1Char('.'));
    if (dot > 0 && dot < raw.size() - 1) {
        const QString market = raw.left(dot).trimmed();
        QString symbol = raw.mid(dot + 1).trimmed();

        if (isDigitsOnly(market) && !symbol.isEmpty()) {
            if (market == QStringLiteral("90")) {
                const QString normalizedSector = normalizeSectorCode(symbol);
                return normalizedSector.isEmpty()
                    ? QString()
                    : (QStringLiteral("90.") + normalizedSector);
            }

            if (market == QStringLiteral("116")) {
                QString hkDigits = digitsOnly(symbol);
                if (hkDigits.isEmpty()) {
                    return {};
                }
                if (hkDigits.size() > 5) {
                    hkDigits = hkDigits.right(5);
                }
                return QStringLiteral("116.")
                    + hkDigits.rightJustified(5, QLatin1Char('0'));
            }

            const QString normalizedIndex = normalizeHongKongIndexCode(
                market + QStringLiteral(".") + symbol
            );
            if (!normalizedIndex.isEmpty()) {
                return normalizedIndex;
            }

            bool hasLetter = false;
            for (const QChar ch : symbol) {
                if (ch.isLetter()) {
                    hasLetter = true;
                    break;
                }
            }
            if (hasLetter) {
                symbol = symbol.toUpper();
            }

            return market + QStringLiteral(".") + symbol;
        }
    }

    const QString lower = raw.toLower();
    if (lower.endsWith(QStringLiteral(".hk")) || lower.startsWith(QStringLiteral("hk"))) {
        QString hkDigits = digitsOnly(lower);
        if (hkDigits.isEmpty()) {
            return {};
        }
        if (hkDigits.size() > 5) {
            hkDigits = hkDigits.right(5);
        }
        return QStringLiteral("116.") + hkDigits.rightJustified(5, QLatin1Char('0'));
    }

    if (raw.size() == 5 && isDigitsOnly(raw)) {
        return QStringLiteral("116.") + raw;
    }

    if (lower.startsWith(QStringLiteral("sh")) || lower.startsWith(QStringLiteral("sz"))) {
        QString digits = digitsOnly(lower.mid(2));
        if (digits.size() > 6) {
            digits = digits.right(6);
        }
        if (digits.size() == 6) {
            const QString market = lower.startsWith(QStringLiteral("sh"))
                ? QStringLiteral("1")
                : QStringLiteral("0");
            return market + QStringLiteral(".") + digits;
        }
        return {};
    }

    if (raw.size() == 6 && isDigitsOnly(raw)) {
        const QChar head = raw.at(0);
        const QString market = (head == QLatin1Char('5')
            || head == QLatin1Char('6')
            || head == QLatin1Char('9'))
            ? QStringLiteral("1")
            : QStringLiteral("0");
        return market + QStringLiteral(".") + raw;
    }

    return {};
}

QString normalizeSectorCode(const QString& rawCode) {
    if (isTdxCode(rawCode) && !isTdxExtendedCode(rawCode) && tdxSymbol(rawCode).startsWith(QStringLiteral("88")))
        return normalizeApiWatchCode(rawCode);
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
    if (isTdxExtendedCode(rawCode)) return normalizeApiWatchCode(rawCode);
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
    if (isTdxCode(rawCode)) return !isTdxExtendedCode(rawCode) && isPredefinedIndexCode(rawCode);
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
    if (isTdxCode(rawCode)) {
        static const QSet<QString> nativeIndexes {
            QStringLiteral("tdx:main:1:000001"), QStringLiteral("tdx:main:0:399001"),
            QStringLiteral("tdx:main:0:399006"), QStringLiteral("tdx:main:1:000688"),
            QStringLiteral("tdx:main:1:000300"), QStringLiteral("tdx:main:1:000905"),
            QStringLiteral("tdx:ex:27:hsi"), QStringLiteral("tdx:ex:27:hz5017")
        };
        return nativeIndexes.contains(watchCodeKey(normalizeApiWatchCode(rawCode)));
    }
    if (code.startsWith(QStringLiteral("bk")) || code.startsWith(QStringLiteral("90."))) {
        return false;
    }
    return predefinedIndexAliases().contains(code);
}

bool isHongKongCode(const QString& rawCode) {
    if (isTdxExtendedCode(rawCode)) {
        const int market = rawCode.trimmed().section(QLatin1Char(':'), 2, 2).toInt();
        return market == 27 || market == 31 || market == 48 || market == 49 || market == 71;
    }
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
        {QStringLiteral("tdx:main:1:600519"), QStringLiteral("Kweichow Moutai")},
        {QStringLiteral("tdx:main:0:000001"), QStringLiteral("Ping An Bank")},
        {QStringLiteral("tdx:main:0:300750"), QStringLiteral("CATL")}
    };
}

bool isCostEditableCode(const QString& rawCode) {
    if (isTdxCode(rawCode) && !isTdxExtendedCode(rawCode)) {
        const int market = rawCode.trimmed().section(QLatin1Char(':'), 2, 2).toInt();
        const QString symbol = tdxSymbol(rawCode);
        return market <= 2 && symbol.size() == 6 && isDigitsOnly(symbol)
            && !isPredefinedIndexCode(rawCode) && !symbol.startsWith(QStringLiteral("88"));
    }
    if (isTdxExtendedCode(rawCode)) {
        const int market = rawCode.trimmed().section(QLatin1Char(':'), 2, 2).toInt();
        return market == 31 || market == 48 || market == 49 || market == 71;
    }
    const QString code = watchCodeKey(rawCode);
    if (code.isEmpty()) {
        return false;
    }

    // A-share stocks: market 0 or 1, 6-digit symbol, not a predefined A-share index
    const int dot = code.indexOf(QLatin1Char('.'));
    if (dot > 0 && dot < code.size() - 1) {
        const QString market = code.left(dot);
        const QString symbol = code.mid(dot + 1);
        if ((market == QLatin1String("0") || market == QLatin1String("1") || market == QLatin1String("2"))
            && symbol.size() == 6
            && isDigitsOnly(symbol)
            && !isPredefinedAshareIndexCode(rawCode)) {
            return true;
        }
        // HK stocks (market 116, 128), not HK indexes
        if ((market == QLatin1String("116") || market == QLatin1String("128"))
            && isDigitsOnly(symbol)) {
            return true;
        }
    }

    // BJ exchange stocks (prefix "bj")
    if (code.startsWith(QLatin1String("bj"))) {
        const QString symbol = code.mid(2);
        return symbol.size() == 6 && isDigitsOnly(symbol);
    }

    return false;
}

} // namespace watchlist_utils
