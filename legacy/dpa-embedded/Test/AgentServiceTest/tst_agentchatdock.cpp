#include <QtTest/QtTest>
#include <QSettings>

#include "AgentTheme.h"
#include "MarkdownLite.h"
#include <string>

static bool isColorToken(const char* tok)
{
    if (!tok || tok[0] != '#') return false;
    const int len = static_cast<int>(strlen(tok));
    if (len != 7) return false;
    for (int i = 1; i < len; ++i) {
        const char c = tok[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

class TstAgentThemeLogic : public QObject
{
    Q_OBJECT
private slots:
    void lightModeColorsAreValid()
    {
        AgentTheme::setDarkMode(false);
        QVERIFY(AgentTheme::isDark() == false);
        QVERIFY(isColorToken(AgentTheme::currentSurfaceBg()));
        QVERIFY(isColorToken(AgentTheme::currentSurfaceCard()));
        QVERIFY(isColorToken(AgentTheme::currentAccentPrimary()));
        QVERIFY(isColorToken(AgentTheme::currentTextPrimary()));
        QVERIFY(isColorToken(AgentTheme::currentTextSecondary()));
        QVERIFY(isColorToken(AgentTheme::currentBorderDefault()));
    }

    void darkModeColorsAreValid()
    {
        AgentTheme::setDarkMode(true);
        QVERIFY(AgentTheme::isDark() == true);
        QVERIFY(isColorToken(AgentTheme::currentSurfaceBg()));
        QVERIFY(isColorToken(AgentTheme::currentSurfaceCard()));
        QVERIFY(isColorToken(AgentTheme::currentAccentPrimary()));
        QVERIFY(isColorToken(AgentTheme::currentTextPrimary()));
        QVERIFY(isColorToken(AgentTheme::currentTextSecondary()));
        QVERIFY(isColorToken(AgentTheme::currentBorderDefault()));
    }

    void lightAndDarkAreDifferent()
    {
        AgentTheme::setDarkMode(false);
        const QString bgLight = AgentTheme::currentSurfaceBg();
        const QString textLight = AgentTheme::currentTextPrimary();

        AgentTheme::setDarkMode(true);
        const QString bgDark = AgentTheme::currentSurfaceBg();
        const QString textDark = AgentTheme::currentTextPrimary();

        QVERIFY(bgLight != bgDark);
        QVERIFY(textLight != textDark);
        AgentTheme::setDarkMode(false);
    }

    void toggleFlipsState()
    {
        const bool initial = AgentTheme::isDark();
        AgentTheme::toggleDarkMode();
        QVERIFY(AgentTheme::isDark() != initial);
        AgentTheme::toggleDarkMode();
        QVERIFY(AgentTheme::isDark() == initial);
    }

    void qSettingsPersistence()
    {
        QSettings s("DPA", "DPA-2026");
        AgentTheme::setDarkMode(true);
        QCOMPARE(s.value("AgentTheme/dark").toBool(), true);
        AgentTheme::setDarkMode(false);
        QCOMPARE(s.value("AgentTheme/dark").toBool(), false);
    }

    void noBlackOnBlack()
    {
        AgentTheme::setDarkMode(false);
        QVERIFY(strcmp(AgentTheme::currentSurfaceBg(), AgentTheme::currentTextPrimary()) != 0);
        AgentTheme::setDarkMode(true);
        QVERIFY(strcmp(AgentTheme::currentSurfaceBg(), AgentTheme::currentTextPrimary()) != 0);
        AgentTheme::setDarkMode(false);
    }
};

class TstMarkdownLiteLogic : public QObject
{
    Q_OBJECT
private slots:
    void tableRendering()
    {
        const std::string md = "| A | B |\n|---|---|\n| 1 | 2 |";
        const std::string html = Model::toRichText(md);
        QVERIFY(html.find("<table") != std::string::npos);
        QVERIFY(html.find("<th") != std::string::npos);
        QVERIFY(html.find("<td") != std::string::npos);
    }

    void codeBlockRendering()
    {
        const std::string md = "```\nint x = 1;\n```";
        const std::string html = Model::toRichText(md);
        QVERIFY(html.find("<pre") != std::string::npos || html.find("<code") != std::string::npos);
    }

    void linkRendering()
    {
        const std::string md = "[click](https://example.com)";
        const std::string html = Model::toRichText(md);
        QVERIFY(html.find("<a") != std::string::npos);
        QVERIFY(html.find("https://example.com") != std::string::npos);
    }

    void boldAndItalic()
    {
        const std::string md = "**bold** and *italic*";
        const std::string html = Model::toRichText(md);
        QVERIFY(html.find("<b>") != std::string::npos || html.find("<strong>") != std::string::npos);
        QVERIFY(html.find("<i>") != std::string::npos || html.find("<em>") != std::string::npos);
    }

    void headingRendering()
    {
        const std::string md = "## Title";
        const std::string html = Model::toRichText(md);
        QVERIFY(html.find("<h") != std::string::npos || html.find("font-weight:bold") != std::string::npos);
    }

    void blockquoteRendering()
    {
        const std::string md = "> quoted text";
        const std::string html = Model::toRichText(md);
        QVERIFY(html.find("border-left") != std::string::npos || html.find("blockquote") != std::string::npos);
    }

    void htmlEscaping()
    {
        const std::string md = "x < 1 & y > 2";
        const std::string html = Model::toRichText(md);
        QVERIFY(html.find("< 1 & y >") == std::string::npos);
    }
};

int runTstAgentChatDock(int argc, char** argv)
{
    int failures = 0;

    {
        TstAgentThemeLogic tc;
        failures += QTest::qExec(&tc, argc, argv);
    }
    {
        TstMarkdownLiteLogic tc;
        failures += QTest::qExec(&tc, argc, argv);
    }

    return failures;
}

#include "tst_agentchatdock.moc"
