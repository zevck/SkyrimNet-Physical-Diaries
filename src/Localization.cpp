#include "Localization.h"
#include "SKSE/SKSE.h"

#include <algorithm>
#include <fstream>
#include <shlobj.h>
#include <cstdio>

namespace SkyrimNetDiaries {

    // ── Language detection ─────────────────────────────────────────────
    // Reads sLanguage from the user's Skyrim.ini.  Returns an uppercase
    // string like "ENGLISH" or "RUSSIAN".
    static std::string DetectSkyrimLanguage() {
        char docsPath[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr,
                                    SHGFP_TYPE_CURRENT, docsPath))) {
            SKSE::log::warn("[Localization] SHGetFolderPath failed; assuming ENGLISH");
            return "ENGLISH";
        }
        std::string iniPath = std::string(docsPath)
                            + "\\My Games\\Skyrim Special Edition\\Skyrim.ini";
        std::ifstream f(iniPath);
        if (!f.is_open()) {
            SKSE::log::warn("[Localization] Could not open '{}'; assuming ENGLISH", iniPath);
            return "ENGLISH";
        }
        std::string line;
        while (std::getline(f, line)) {
            auto ns = line.find_first_not_of(" \t");
            if (ns == std::string::npos) continue;
            line = line.substr(ns);
            if (line.empty() || line[0] == ';' || line[0] == '[') continue;
            if (line.size() >= 9 && line.substr(0, 9) == "sLanguage") {
                auto eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string val = line.substr(eq + 1);
                    while (!val.empty() && (val.back() == ' ' || val.back() == '\r'
                                        || val.back() == '\n' || val.back() == '\t'))
                        val.pop_back();
                    auto vs = val.find_first_not_of(" \t");
                    if (vs != std::string::npos) val = val.substr(vs);
                    std::transform(val.begin(), val.end(), val.begin(), ::toupper);
                    return val;
                }
            }
        }
        return "ENGLISH";
    }

    static Language ParseLanguage(const std::string& lang) {
        if (lang == "FRENCH")    return Language::French;
        if (lang == "GERMAN")    return Language::German;
        if (lang == "ITALIAN")   return Language::Italian;
        if (lang == "SPANISH")   return Language::Spanish;
        if (lang == "POLISH")    return Language::Polish;
        if (lang == "RUSSIAN")   return Language::Russian;
        if (lang == "CHINESE")   return Language::ChineseTraditional;
        if (lang == "JAPANESE")  return Language::Japanese;
        return Language::English;
    }

    // ── Chinese numeral helper ─────────────────────────────────────────
    static std::string ChineseNumeral(int n) {
        static const char* digits[] = {
            "", "\xe4\xb8\x80", "\xe4\xba\x8c", "\xe4\xb8\x89", "\xe5\x9b\x9b",
            "\xe4\xba\x94", "\xe5\x85\xad", "\xe4\xb8\x83", "\xe5\x85\xab", "\xe4\xb9\x9d"
        };  // 一二三四五六七八九
        static const char* ten = "\xe5\x8d\x81"; // 十

        if (n <= 0) return std::to_string(n);
        if (n < 10) return digits[n];
        if (n == 10) return ten;
        if (n < 20) return std::string(ten) + digits[n - 10];
        if (n < 100) {
            std::string result = std::string(digits[n / 10]) + ten;
            if (n % 10 != 0) result += digits[n % 10];
            return result;
        }
        return std::to_string(n);  // Fallback for 100+
    }

    // ── English ordinal suffix ─────────────────────────────────────────
    static std::string OrdinalSuffix(int n) {
        if (n % 100 >= 11 && n % 100 <= 13) return "th";
        switch (n % 10) {
            case 1: return "st";
            case 2: return "nd";
            case 3: return "rd";
            default: return "th";
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    //  Locale definitions
    // ═══════════════════════════════════════════════════════════════════

    // ── English ────────────────────────────────────────────────────────
    static const LocaleData kLocaleEnglish = {
        // monthNames
        {"Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand",
         "Second Seed", "Midyear", "Sun's Height", "Last Seed",
         "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star"},
        // dayNames
        {"Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"},
        // formatDateLong: "Sundas, 17 Last Seed, 4E 201"
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s, %d %s, 4E %d", day, d, month, y);
            return buf;
        },
        // formatDateShort: "17 Last Seed, 4E 201"
        [](int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d %s, 4E %d", d, month, y);
            return buf;
        },
        // formatDiaryTitle
        [](const std::string& name) -> std::string { return name + "'s Diary"; },
        // formatVolumeSuffix
        [](int vol) -> std::string {
            return (vol > 1) ? ", v" + std::to_string(vol) : "";
        },
        // emptyVolumeText
        "All entries from this time period have been removed."
    };

    // ── French ─────────────────────────────────────────────────────────
    static const LocaleData kLocaleFrench = {
        {"Prim\u00e9toile", "Clairciel", "Semailles", "Ondepluie",
         "Plantaisons", "Mi-l'An", "Hautz\u00e9nith", "Vifazur",
         "Atrefeu", "Soufflegivre", "Sombreciel", "Soir\u00e9toile"},
        {"Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"},
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s, %d %s, 4E %d", day, d, month, y);
            return buf;
        },
        [](int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d %s, 4E %d", d, month, y);
            return buf;
        },
        [](const std::string& name) -> std::string { return "Journal de " + name; },
        [](int vol) -> std::string {
            return (vol > 1) ? ", vol. " + std::to_string(vol) : "";
        },
        "Toutes les entr\u00e9es de cette p\u00e9riode ont \u00e9t\u00e9 supprim\u00e9es."
    };

    // ── German ─────────────────────────────────────────────────────────
    static const LocaleData kLocaleGerman = {
        {"Morgenstern", "Sonnenaufgang", "Erste Saat", "Regenhand",
         "Zweite Saat", "Jahresmitte", "Sonnenh\u00f6he", "Letzte Saat",
         "Herzfeuer", "Eisherbst", "Sonnenuntergang", "Abendstern"},
        {"Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"},
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s, %d %s, 4E %d", day, d, month, y);
            return buf;
        },
        [](int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d %s, 4E %d", d, month, y);
            return buf;
        },
        [](const std::string& name) -> std::string { return name + "s Tagebuch"; },
        [](int vol) -> std::string {
            return (vol > 1) ? ", Buch " + std::to_string(vol) : "";
        },
        "Alle Eintr\u00e4ge aus diesem Zeitraum wurden entfernt."
    };

    // ── Italian ────────────────────────────────────────────────────────
    static const LocaleData kLocaleItalian = {
        {"Stella del Mattino", "Luce dell'Alba", "Primo Seme", "Mano della Pioggia",
         "Secondo Seme", "Met\u00e0 Annata", "Luce del Cielo", "Ultimo Seme",
         "Focolare", "Gelata", "Luce del Crepuscolo", "Stella della Sera"},
        {"Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"},
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s, %d %s, 4E %d", day, d, month, y);
            return buf;
        },
        [](int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d %s, 4E %d", d, month, y);
            return buf;
        },
        [](const std::string& name) -> std::string { return "Diario di " + name; },
        [](int vol) -> std::string {
            return (vol > 1) ? " - Vol. " + std::to_string(vol) : "";
        },
        "Tutte le voci di questo periodo sono state rimosse."
    };

    // ── Spanish ────────────────────────────────────────────────────────
    static const LocaleData kLocaleSpanish = {
        {"Estrella del alba", "Amanecer", "Primera semilla", "Mano de lluvia",
         "Segunda semilla", "Mitad de a\u00f1o", "Culminaci\u00f3n solar", "\u00daltima semilla",
         "Fuego hogar", "Oto\u00f1o de escarcha", "Ocaso", "Estrella vespertina"},
        {"Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"},
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s, %d de %s, 4E %d", day, d, month, y);
            return buf;
        },
        [](int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d de %s, 4E %d", d, month, y);
            return buf;
        },
        [](const std::string& name) -> std::string { return "Diario de " + name; },
        [](int vol) -> std::string {
            return (vol > 1) ? ", vol. " + std::to_string(vol) : "";
        },
        "Todas las entradas de este periodo han sido eliminadas."
    };

    // ── Polish ─────────────────────────────────────────────────────────
    static const LocaleData kLocalePolish = {
        {"Gwiazda Por.", "Wsch. S\u0142o\u0144ce", "Pierw. Siew", "Deszcz. D\u0142o\u0144",
         "Drugi Siew", "\u015ar\u00f3drocze", "Pe\u0142nia S\u0142o\u0144ca", "Ostatni Siew",
         "Dom. ognisko", "Pierw. Mrozy", "Zach. S\u0142o\u0144ce", "Gwiazda Wiecz."},
        {"Sundas", "Morndas", "Tirdas", "Middas", "Turdas", "Fredas", "Loredas"},
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s, %d %s, 4E %d", day, d, month, y);
            return buf;
        },
        [](int d, const char* month, int y) -> std::string {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d %s, 4E %d", d, month, y);
            return buf;
        },
        [](const std::string& name) -> std::string { return "Dziennik: " + name; },
        [](int vol) -> std::string {
            return (vol > 1) ? std::string(", ksi\u0119ga ") + std::to_string(vol) : "";
        },
        "Wszystkie wpisy z tego okresu zosta\u0142y usuni\u0119te."
    };

    // ── Russian ────────────────────────────────────────────────────────
    static const LocaleData kLocaleRussian = {
        {"\u0423\u0442\u0440\u0435\u043d\u043d\u0435\u0439 \u0437\u0432\u0435\u0437\u0434\u044b",          // Утренней звезды
         "\u0412\u043e\u0441\u0445\u043e\u0434\u0430 \u0441\u043e\u043b\u043d\u0446\u0430",                  // Восхода солнца
         "\u041f\u0435\u0440\u0432\u043e\u0433\u043e \u0437\u0435\u0440\u043d\u0430",                        // Первого зерна
         "\u0420\u0443\u043a\u0438 \u0434\u043e\u0436\u0434\u044f",                                          // Руки дождя
         "\u0412\u0442\u043e\u0440\u043e\u0433\u043e \u0437\u0435\u0440\u043d\u0430",                        // Второго зерна
         "\u0421\u0435\u0440\u0435\u0434\u0438\u043d\u044b \u0433\u043e\u0434\u0430",                        // Середины года
         "\u0412\u044b\u0441\u043e\u043a\u043e\u0433\u043e \u0441\u043e\u043b\u043d\u0446\u0430",            // Высокого солнца
         "\u041f\u043e\u0441\u043b\u0435\u0434\u043d\u0435\u0433\u043e \u0437\u0435\u0440\u043d\u0430",      // Последнего зерна
         "\u041e\u0433\u043d\u044f \u043e\u0447\u0430\u0433\u0430",                                          // Огня очага
         "\u041d\u0430\u0447\u0430\u043b\u0430 \u043c\u043e\u0440\u043e\u0437\u043e\u0432",                  // Начала морозов
         "\u0417\u0430\u043a\u0430\u0442\u0430 \u0441\u043e\u043b\u043d\u0446\u0430",                        // Заката солнца
         "\u0412\u0435\u0447\u0435\u0440\u043d\u0435\u0439 \u0437\u0432\u0435\u0437\u0434\u044b"},           // Вечерней звезды
        // dayNames: Cyrillic transliterations
        {"\u0421\u0430\u043d\u0434\u0430\u0441",     // Сандас
         "\u041c\u043e\u0440\u043d\u0434\u0430\u0441", // Морндас
         "\u0422\u0438\u0440\u0434\u0430\u0441",     // Тирдас
         "\u041c\u0438\u0434\u0434\u0430\u0441",     // Миддас
         "\u0422\u0443\u0440\u0434\u0430\u0441",     // Турдас
         "\u0424\u0440\u0435\u0434\u0430\u0441",     // Фредас
         "\u041b\u043e\u0440\u0435\u0434\u0430\u0441"}, // Лоредас
        // formatDateLong: "Сандас, 17 день месяца Последнего зерна, 4Э 201"
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s, %d \u0434\u0435\u043d\u044c \u043c\u0435\u0441\u044f\u0446\u0430 %s, 4\u042d %d",
                     day, d, month, y);
            return buf;
        },
        // formatDateShort: "17 день месяца Последнего зерна, 4Э 201"
        [](int d, const char* month, int y) -> std::string {
            char buf[256];
            snprintf(buf, sizeof(buf), "%d \u0434\u0435\u043d\u044c \u043c\u0435\u0441\u044f\u0446\u0430 %s, 4\u042d %d",
                     d, month, y);
            return buf;
        },
        // formatDiaryTitle: "Дневник: Name"
        [](const std::string& name) -> std::string {
            return std::string("\u0414\u043d\u0435\u0432\u043d\u0438\u043a: ") + name;
        },
        // formatVolumeSuffix: ", т. N"
        [](int vol) -> std::string {
            return (vol > 1) ? std::string(", \u0442. ") + std::to_string(vol) : "";
        },
        // emptyVolumeText
        "\u0412\u0441\u0435 \u0437\u0430\u043f\u0438\u0441\u0438 \u0437\u0430 \u044d\u0442\u043e\u0442 \u043f\u0435\u0440\u0438\u043e\u0434 \u0431\u044b\u043b\u0438 \u0443\u0434\u0430\u043b\u0435\u043d\u044b."  // Все записи за этот период были удалены.
    };

    // ── Traditional Chinese ────────────────────────────────────────────
    static const LocaleData kLocaleChinese = {
        {"\u6668\u661f\u6708",     // 晨星月
         "\u65e5\u66d9\u6708",     // 日曙月
         "\u521d\u7a2e\u6708",     // 初種月
         "\u96e8\u624b\u6708",     // 雨手月
         "\u6b21\u7a2e\u6708",     // 次種月
         "\u5e74\u4e2d\u6708",     // 年中月
         "\u65e5\u9ad8\u6708",     // 日高月
         "\u672b\u7a2e\u6708",     // 末種月
         "\u7210\u706b\u6708",     // 爐火月
         "\u971c\u843d\u6708",     // 霜落月
         "\u65e5\u66ae\u6708",     // 日暮月
         "\u591c\u661f\u6708"},    // 夜星月
        // dayNames
        {"\u9031\u65e5",   // 週日
         "\u9031\u4e00",   // 週一
         "\u9031\u4e8c",   // 週二
         "\u9031\u4e09",   // 週三
         "\u9031\u56db",   // 週四
         "\u9031\u4e94",   // 週五
         "\u9031\u516d"},  // 週六
        // formatDateLong: "末種月第17天，第四紀元 201年，週日"
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s\u7b2c%d\u5929\uff0c\u7b2c\u56db\u7d00\u5143 %d\u5e74\uff0c%s",
                     month, d, y, day);
            return buf;
        },
        // formatDateShort: "末種月第17天，第四紀元 201年"
        [](int d, const char* month, int y) -> std::string {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s\u7b2c%d\u5929\uff0c\u7b2c\u56db\u7d00\u5143 %d\u5e74",
                     month, d, y);
            return buf;
        },
        // formatDiaryTitle: "Name的日記"
        [](const std::string& name) -> std::string {
            return name + "\u7684\u65e5\u8a18";
        },
        // formatVolumeSuffix: "，卷二"  (uses Chinese numerals)
        [](int vol) -> std::string {
            return (vol > 1) ? std::string("\uff0c\u5377") + ChineseNumeral(vol) : "";
        },
        // emptyVolumeText
        "\u6b64\u6642\u671f\u7684\u6240\u6709\u689d\u76ee\u5df2\u88ab\u522a\u9664\u3002"  // 此時期的所有條目已被刪除。
    };

    // ── Japanese ───────────────────────────────────────────────────────
    static const LocaleData kLocaleJapanese = {
        {"\u6681\u661f\u306e\u6708",         // 暁星の月
         "\u8584\u660e\u306e\u6708",         // 薄明の月
         "\u8494\u7a2e\u306e\u6708",         // 蒔種の月
         "\u30ec\u30a4\u30f3\u30ba\u30fb\u30cf\u30f3\u30c9", // レインズ・ハンド
         "\u683d\u57f9\u306e\u6708",         // 栽培の月
         "\u5e74\u592e",                     // 年央
         "\u5357\u4e2d\u306e\u6708",         // 南中の月
         "\u53ce\u7a6b\u306e\u6708",         // 収穫の月
         "\u85aa\u6728\u306e\u6708",         // 薪木の月
         "\u964d\u971c\u306e\u6708",         // 降霜の月
         "\u9ec4\u660f\u306e\u6708",         // 黄昏の月
         "\u661f\u971c\u306e\u6708"},        // 星霜の月
        // dayNames: single kanji
        {"\u65e5",   // 日
         "\u6708",   // 月
         "\u706b",   // 火
         "\u6c34",   // 水
         "\u6728",   // 木
         "\u91d1",   // 金
         "\u571f"},  // 土
        // formatDateLong: "第四紀201年 収穫の月17日 日"
        [](const char* day, int d, const char* month, int y) -> std::string {
            char buf[256];
            snprintf(buf, sizeof(buf), "\u7b2c\u56db\u7d00%d\u5e74 %s%d\u65e5 %s",
                     y, month, d, day);
            return buf;
        },
        // formatDateShort: "第四紀201年 収穫の月17日"
        [](int d, const char* month, int y) -> std::string {
            char buf[256];
            snprintf(buf, sizeof(buf), "\u7b2c\u56db\u7d00%d\u5e74 %s%d\u65e5",
                     y, month, d);
            return buf;
        },
        // formatDiaryTitle: "Nameの日記"
        [](const std::string& name) -> std::string {
            return name + "\u306e\u65e5\u8a18";
        },
        // formatVolumeSuffix: " 第2巻"
        [](int vol) -> std::string {
            return (vol > 1) ? std::string(" \u7b2c") + std::to_string(vol) + "\u5dfb" : "";
        },
        // emptyVolumeText
        "\u3053\u306e\u671f\u9593\u306e\u3059\u3079\u3066\u306e\u8a18\u4e8b\u306f\u524a\u9664\u3055\u308c\u307e\u3057\u305f\u3002"  // この期間のすべての記事は削除されました。
    };

    // ═══════════════════════════════════════════════════════════════════
    //  Locale table
    // ═══════════════════════════════════════════════════════════════════

    static const LocaleData* GetLocaleForLanguage(Language lang) {
        switch (lang) {
            case Language::French:              return &kLocaleFrench;
            case Language::German:              return &kLocaleGerman;
            case Language::Italian:             return &kLocaleItalian;
            case Language::Spanish:             return &kLocaleSpanish;
            case Language::Polish:              return &kLocalePolish;
            case Language::Russian:             return &kLocaleRussian;
            case Language::ChineseTraditional:  return &kLocaleChinese;
            case Language::Japanese:            return &kLocaleJapanese;
            default:                            return &kLocaleEnglish;
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    //  Public API
    // ═══════════════════════════════════════════════════════════════════

    void Localization::Initialize() {
        languageString_ = DetectSkyrimLanguage();
        language_ = ParseLanguage(languageString_);
        activeLocale_ = GetLocaleForLanguage(language_);
        SKSE::log::info("[Localization] Detected language: '{}' -> locale initialized", languageString_);
    }

    std::string Localization::FormatBookName(const std::string& actorName, int volumeNumber) const {
        std::string name = activeLocale_->formatDiaryTitle(actorName);
        name += activeLocale_->formatVolumeSuffix(volumeNumber);
        return name;
    }

}  // namespace SkyrimNetDiaries
