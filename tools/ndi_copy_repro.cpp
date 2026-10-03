// tools/ndi_copy_repro.cpp — minimal reproduction of the
// qml/services/LiveOutputService.cpp build failure, kept so the workaround
// there is not "magic" to a future reader.
//
// COMPILER: GCC 15.2 (MSYS2 UCRT64, MinGW-w64), -std=c++26, Qt 6.11.1.
//
// SYMPTOM: a prvalue QImage followed by `.copy()` fails to parse —
//
//   QImage owned(p, w, h, bpl, QImage::Format_RGBA8888).copy();
//   error: expected ',' or ';' before '.' token
//
// It is NOT a missing header, a macro, or a nodiscard problem: the same line
// is accepted once the temporary has a NAME (or is bound to a reference, or
// wrapped in extra parens). The workaround in LiveOutputService.cpp names the
// wrap first; behaviour is identical (the named QImage is a non-owning wrap of
// the engine's buffer, copy() is the deep copy).
//
// BUILD CHECK (from the repo root, with tools/agent_env.sh sourced):
//   g++ -std=c++26 -fsyntax-only \
//     -isystem C:/Qt/6.11.1/mingw_64/include \
//     -isystem C:/Qt/6.11.1/mingw_64/include/QtCore \
//     -isystem C:/Qt/6.11.1/mingw_64/include/QtGui \
//     -isystem C:/Qt/6.11.1/mingw_64/mkspecs/win32-g++ \
//     tools/ndi_copy_repro.cpp
// Must exit 0. Un-commenting brokenForm() must make it fail.
#include <QImage>

void namedForm(const uchar *p, int w, int h, int bpl)
{
    // Works: the temporary is materialised into `wrapped` first.
    const QImage wrapped(p, w, h, bpl, QImage::Format_RGBA8888);
    QImage owned = wrapped.copy();
    (void)owned;
}

void referenceForm(const uchar *p, int w, int h, int bpl)
{
    // Works: binding to a reference gives the prvalue a name.
    auto &&tmp = QImage(p, w, h, bpl, QImage::Format_RGBA8888);
    QImage owned = tmp.copy();
    (void)owned;
}

// void brokenForm(const uchar *p, int w, int h, int bpl)
// {
//     // DOES NOT COMPILE on GCC 15.2 MinGW — the original line.
//     QImage owned(p, w, h, bpl, QImage::Format_RGBA8888).copy();
//     (void)owned;
// }
