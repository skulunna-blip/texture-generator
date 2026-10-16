// Regression check for dropping an image onto the window.
//
// This drives the real MainWindow with synthetic drag events rather than
// calling the loading code directly, because the part worth protecting is the
// wiring: that the window accepts drops at all, that it recognises a file it
// can read, that it refuses one it cannot, and that a drop ends up in the same
// state the Choose Image button leaves behind. Every one of those is a thing a
// refactor can quietly break without any compiler complaint.
#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImage>
#include <QLabel>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>

namespace
{
int failures = 0;

void check(bool ok, const char* what, const QString& detail = QString())
{
    const QByteArray tail = detail.isEmpty() ? QByteArray() : (QStringLiteral("  ") + detail).toUtf8();
    std::printf("DROP %-34s %s%s\n", what, ok ? "PASS" : "FAIL", tail.constData());
    std::fflush(stdout);
    if (!ok) {
        ++failures;
    }
}

// The source preview is the window's own report of whether a source is loaded:
// it shows the image when there is one and the placeholder text when there is
// not. Reading it keeps the test out of MainWindow's private state.
QLabel* sourcePreview(QWidget* window)
{
    return window->findChild<QLabel*>(QStringLiteral("previewSurface"));
}

bool sourceLoaded(QWidget* window)
{
    QLabel* label = sourcePreview(window);
    return label && !label->pixmap().isNull();
}

// A drop arrives as two events: an enter, which decides whether the window is
// interested, and the drop itself. Sending them in order is what the platform
// does, and the enter is where the highlight comes from.
bool drop(QWidget* window, const QMimeData* mime)
{
    const QPointF at(40, 40);
    QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(window, &enter);
    if (!enter.isAccepted()) {
        return false;
    }
    QDropEvent dropped(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(window, &dropped);
    return dropped.isAccepted();
}

QMimeData* urlsFor(const QStringList& paths)
{
    QList<QUrl> urls;
    for (const QString& path : paths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    auto* mime = new QMimeData;
    mime->setUrls(urls);
    return mime;
}
} // namespace

int main(int argc, char** argv)
{
    // The window is built but never shown, so nothing appears on screen. It is
    // deliberately not run on the offscreen platform: the preview widgets are
    // OpenGL, and offscreen would make them a different code path from the one
    // users actually get.
    QApplication app(argc, argv);

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("DROP could not create a temp dir\n");
        return 1;
    }

    const QString imagePath = dir.filePath(QStringLiteral("source.png"));
    QImage sample(64, 48, QImage::Format_RGB32);
    sample.fill(qRgb(30, 90, 150));
    sample.save(imagePath);

    const QString textPath = dir.filePath(QStringLiteral("notes.txt"));
    {
        QFile notes(textPath);
        notes.open(QIODevice::WriteOnly);
        notes.write("not an image");
    }

    MainWindow window;
    // The handlers below are driven by sending events straight to the window,
    // which bypasses the flag that decides whether the platform routes a real
    // drag here at all. So that flag is checked on its own. It currently holds
    // because QMainWindow accepts drops by default; this pins the requirement
    // rather than leaving it to be discovered by a user whose drop silently
    // does nothing.
    check(window.acceptDrops(), "window accepts drops");
    check(sourcePreview(&window) != nullptr, "source preview exists");
    check(!sourceLoaded(&window), "no source at startup");

    // --- 1. A dropped image loads -----------------------------------------
    {
        QMimeData* mime = urlsFor({imagePath});
        const bool accepted = drop(&window, mime);
        check(accepted && sourceLoaded(&window), "dropped image loads",
              accepted ? QString() : QStringLiteral("enter was refused"));
        delete mime;
    }

    // --- 2. A drop that cannot be read is refused -------------------------
    // Refusing at the enter stage is what stops the window highlighting itself
    // as a target for a file it will only reject.
    {
        QMimeData* mime = urlsFor({textPath});
        const QPointF at(40, 40);
        QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &enter);
        check(!enter.isAccepted(), "text file refused on enter");
        delete mime;
    }

    // --- 3. A mixed drop picks the image ----------------------------------
    // Dragging a folder's worth of files onto the window is normal; the drop
    // should do the obvious thing rather than fail because of the strays.
    {
        QMimeData* mime = urlsFor({textPath, imagePath});
        const bool accepted = drop(&window, mime);
        check(accepted && sourceLoaded(&window), "mixed drop picks the image");
        delete mime;
    }

    // --- 4. A drop with no files at all is refused ------------------------
    {
        QMimeData mime;
        mime.setText(QStringLiteral("just some text"));
        const QPointF at(40, 40);
        QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &enter);
        check(!enter.isAccepted(), "non-file drop refused");
    }

    // --- 5. The highlight follows the drag --------------------------------
    // The cue is a dynamic property driving a stylesheet rule, so it is worth
    // proving it is set on enter and cleared again - both on a drop and on a
    // drag that leaves without dropping.
    {
        QMimeData* mime = urlsFor({imagePath});
        const QPointF at(40, 40);
        QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &enter);
        const bool litOnEnter = sourcePreview(&window)->property("dropTarget").toBool();

        QDragLeaveEvent leave;
        QApplication::sendEvent(&window, &leave);
        const bool clearedOnLeave = !sourcePreview(&window)->property("dropTarget").toBool();
        check(litOnEnter && clearedOnLeave, "highlight follows the drag");

        QApplication::sendEvent(&window, &enter);
        const bool litAgain = sourcePreview(&window)->property("dropTarget").toBool();
        drop(&window, mime);
        const bool clearedOnDrop = !sourcePreview(&window)->property("dropTarget").toBool();
        check(litAgain && clearedOnDrop, "highlight cleared on drop");
        delete mime;
    }

    // --- 6. A drop on a child widget still reaches the window -------------
    // This is the path a user actually takes: nobody aims for the window
    // frame, they drop onto whatever is under the cursor, which is one of the
    // panels. If the window only handled drops that landed on itself the
    // feature would look broken in practice while passing everything above.
    {
        QWidget* child = sourcePreview(&window);
        QMimeData* mime = urlsFor({imagePath});
        const QPointF at(40, 40);
        QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(child, &enter);
        const bool reached = enter.isAccepted();
        QDropEvent dropped(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(child, &dropped);
        check(reached && sourceLoaded(&window), "drop on a child reaches the window");
        delete mime;
    }

    std::printf("DROP %s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures,
                failures == 1 ? "" : "s");
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
