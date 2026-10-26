#include <QtTest/QtTest>
#include "MarkdownLite.h"
using namespace Model;
class TstMarkdownLite : public QObject { Q_OBJECT
    static QString r(const std::string& s){ return QString::fromStdString(toRichText(s)); }
private slots:
    void bold() { QVERIFY(r("**hi**").contains("<b>hi</b>")); }
    void code() { QVERIFY(r("`x`").contains("<code")); QVERIFY(r("`x`").contains("x</code>")); }
    void heading() { QVERIFY(r("# Title").contains("Title")); QVERIFY(r("# Title").contains("font-weight:bold")); }
    void heading_levels() {
        QString h1 = r("# H1"); QString h3 = r("### H3");
        QVERIFY(h1.contains("H1")); QVERIFY(h3.contains("H3"));
        // H1 should have larger font than H3
        QVERIFY(h1.contains("20px") || h1.contains("font-size"));
    }
    void list() {
        QString o = r("- a\n- b");
        QVERIFY(o.contains("<ul")); QVERIFY(o.contains("<li>a</li>")); QVERIFY(o.contains("<li>b</li>")); QVERIFY(o.contains("</ul>"));
    }
    void numbered_list() {
        QString o = r("1. first\n2. second");
        QVERIFY(o.contains("1.")); QVERIFY(o.contains("first")); QVERIFY(o.contains("second"));
    }
    void escapes_html() {
        QString o = r("a < b & c > d");
        QVERIFY(o.contains("&lt;")); QVERIFY(o.contains("&amp;")); QVERIFY(o.contains("&gt;"));
        QVERIFY(!o.contains("a < b"));
    }
    void mixed_paragraph_then_list() {
        QString o = r("诊断结果:\n- 不对中\n- 轴承外圈");
        QVERIFY(o.contains("诊断结果")); QVERIFY(o.contains("<li>不对中</li>"));
    }
    void unbalanced_bold_is_safe() { QVERIFY(r("**oops").contains("<b>oops</b>")); }
    void link() {
        QString o = r("[Google](https://google.com)");
        QVERIFY(o.contains("<a href=")); QVERIFY(o.contains("https://google.com")); QVERIFY(o.contains("Google"));
    }
    void code_block() {
        QString o = r("```\nint x = 1;\n```");
        QVERIFY(o.contains("<pre")); QVERIFY(o.contains("int x = 1;"));
    }
    void code_block_with_lang() {
        QString o = r("```cpp\nauto x = 1;\n```");
        QVERIFY(o.contains("cpp")); QVERIFY(o.contains("auto x = 1;"));
    }
    void blockquote() {
        QString o = r("> important note");
        QVERIFY(o.contains("important note")); QVERIFY(o.contains("border-left"));
    }
    void horizontal_rule() {
        QString o = r("before\n---\nafter");
        QVERIFY(o.contains("<hr")); QVERIFY(o.contains("before")); QVERIFY(o.contains("after"));
    }
    void table() {
        QString o = r("| A | B |\n|---|---|\n| 1 | 2 |");
        QVERIFY2(o.contains("<table"), qPrintable("Missing <table>: " + o));
        QVERIFY2(o.contains("<th"), qPrintable("Missing <th>: " + o));
        QVERIFY2(o.contains("A"), qPrintable("Missing A: " + o));
        QVERIFY2(o.contains("<td"), qPrintable("Missing <td>: " + o));
        QVERIFY2(o.contains("1"), qPrintable("Missing 1: " + o));
        QVERIFY2(o.contains("2"), qPrintable("Missing 2: " + o));
    }
    void inline_italic() {
        QString o = r("*italic*");
        QVERIFY(o.contains("<i>italic</i>"));
    }
};
int runTstMarkdownLite(int argc,char**argv){ TstMarkdownLite t; return QTest::qExec(&t,argc,argv); }
#include "tst_markdownlite.moc"
