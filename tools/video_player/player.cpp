#ifndef DART_TOOLS_VIDEO_PLAYER_PLAYER_CPP
#define DART_TOOLS_VIDEO_PLAYER_PLAYER_CPP

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QImageReader>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include "image_view.cpp"
#include "recording.cpp"

namespace dart::tools::video_player {

class Player final : public QMainWindow {
public:
    explicit Player(Recording recording) : recording_(std::move(recording)) {
        setWindowTitle(QStringLiteral("Dart recording player"));
        resize(1200, 760);
        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        auto* summary = new QLabel(recording_.summary(), central);
        summary->setTextFormat(Qt::PlainText);
        summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(summary);
        auto* splitter = new QSplitter(Qt::Horizontal, central);
        view_ = new ImageView(splitter);
        view_->setObjectName(QStringLiteral("image_view"));
        info_ = new QPlainTextEdit(splitter);
        info_->setObjectName(QStringLiteral("frame_info"));
        info_->setReadOnly(true);
        info_->setMinimumWidth(310);
        splitter->addWidget(view_);
        splitter->addWidget(info_);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        layout->addWidget(splitter, 1);
        slider_ = new QSlider(Qt::Horizontal, central);
        slider_->setObjectName(QStringLiteral("frame_slider"));
        slider_->setRange(0, static_cast<int>(recording_.frames().size()) - 1);
        layout->addWidget(slider_);
        setCentralWidget(central);
        connect(slider_, &QSlider::valueChanged, this, [this](int value) { show_frame(value); });
        view_->clicked = [this](int x, int y) {
            const QString pixel = QStringLiteral("pixel (%1, %2)").arg(x).arg(y);
            info_->setPlainText(details_ + QStringLiteral("\nClicked ") + pixel);
            statusBar()->showMessage(pixel);
        };
        for (const auto key : {Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End}) {
            auto* shortcut = new QShortcut(QKeySequence(key), this);
            connect(shortcut, &QShortcut::activated, this, [this, key] {
                int value = slider_->value();
                if (key == Qt::Key_Home)
                    value = slider_->minimum();
                else if (key == Qt::Key_End)
                    value = slider_->maximum();
                else
                    value += key == Qt::Key_Left ? -1 : 1;
                slider_->setValue(std::clamp(value, slider_->minimum(), slider_->maximum()));
            });
        }
        show_frame(0);
    }

private:
    void show_frame(int index) {
        if (index < 0 || index >= static_cast<int>(recording_.frames().size()))
            return;
        const auto& frame = recording_.frames().at(static_cast<std::size_t>(index));
        details_ = QStringLiteral("Frame %1/%2\nimage_index: %3\nfile: %4\n")
            .arg(index + 1).arg(recording_.frames().size()).arg(frame.image_index).arg(frame.path);
        if (!recording_.notes().isEmpty())
            details_ += '\n' + recording_.notes().join('\n') + '\n';
        if (!frame.notes.isEmpty())
            details_ += '\n' + frame.notes.join('\n') + '\n';
        if (frame.event)
            details_ += QStringLiteral("\nRECORDER\n") + frame.event->text();
        if (frame.detection)
            details_ += QStringLiteral("\nDETECTION (exact frame identity)\n")
                + frame.detection->text();

        QImageReader reader(frame.path);
        const QImage image = reader.read();
        if (image.isNull()) {
            view_->set_frame({}); // Never retain the previous image under the next frame's log.
            details_ += QStringLiteral("\nIMAGE ERROR: ") + reader.errorString();
        } else {
            view_->set_frame(image, ImageView::overlay_for(frame, image.size()));
            details_ += QStringLiteral("\nsize: %1 x %2\nGreen: valid target; cyan: scan window.")
                .arg(image.width()).arg(image.height());
        }
        info_->setPlainText(details_);
        statusBar()->showMessage(QStringLiteral("Frame %1/%2 — Left/Right, Home/End; click pixel")
            .arg(index + 1).arg(recording_.frames().size()));
    }

    Recording recording_;
    ImageView* view_ = nullptr;
    QPlainTextEdit* info_ = nullptr;
    QSlider* slider_ = nullptr;
    QString details_;
};

inline int run_application(int argc, char** argv) {
    // Parse/help/check need no window system. Construct QApplication only for the viewer.
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Host frame viewer for v2 rec_NNNN/{img,logs/frames.csv}.\n"
        "Port of k230_v1 tools/video_player; manual stepping, no timed playback."));
    parser.addHelpOption();
    parser.addOption(QCommandLineOption({"f", "pbm"}, "Show only frame_*.pbm."));
    parser.addOption(QCommandLineOption({"r", "pgm"}, "Show only frame_*.pgm."));
    parser.addOption(QCommandLineOption(
        "check", "Check metadata associations without opening GUI."));
    parser.addPositionalArgument("session", "A rec_NNNN session or its img directory.");
    parser.addPositionalArgument("frames.csv", "Recorder CSV (default: session/logs/frames.csv).",
                                 "[frames.csv]");
    parser.addPositionalArgument("detection.csv", "Detection CSV (default: records/detection.csv).",
                                 "[detection.csv]");
    QStringList args;
    for (int i = 0; i < argc; ++i)
        args.push_back(QString::fromLocal8Bit(argv[i]));
    const bool parsed = parser.parse(args);
    const auto positional = parser.positionalArguments();
    const bool valid = !positional.isEmpty() && positional.size() <= 3
        && !(parser.isSet("f") && parser.isSet("r"));
    const bool wants_help = parser.isSet("help") || parser.isSet("help-all");
    if (!parsed || wants_help || !valid) {
        QCoreApplication app(argc, argv); // helpText needs an application, but no GUI plugin.
        if (!parsed)
            QTextStream(stderr) << parser.errorText() << '\n';
        const bool help = parsed && wants_help;
        QTextStream(help ? stdout : stderr) << parser.helpText();
        return help ? 0 : 2;
    }
    std::unique_ptr<QCoreApplication> application;
    if (parser.isSet("check"))
        application = std::make_unique<QCoreApplication>(argc, argv);
    const QDir input(QFileInfo(positional.at(0)).absoluteFilePath());
    const QDir images(input.exists("img") ? input.filePath("img") : input.absolutePath());
    QDir session(images);
    session.cdUp();
    QDir records(session);
    records.cdUp();
    const QString frames_csv = positional.size() > 1 ? positional.at(1)
        : session.filePath("logs/frames.csv");
    const QString detection_csv = positional.size() > 2 ? positional.at(2)
        : records.filePath("detection.csv");
    for (int i = 1; i < positional.size(); ++i) {
        if (!QFileInfo::exists(positional.at(i))) {
            QTextStream(stderr) << positional.at(i) << ": explicit CSV does not exist\n";
            return 2;
        }
    }
    Recording recording;
    QString error;
    const QString extension = parser.isSet("f") ? "pbm" : parser.isSet("r") ? "pgm" : "";
    if (!recording.load(images.absolutePath(), frames_csv, detection_csv, extension, error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    if (recording.frames().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        QTextStream(stderr) << "Too many images for the frame slider\n";
        return 1;
    }
    QTextStream(stdout) << recording.summary() << '\n';
    for (const auto& note : recording.notes())
        QTextStream(stderr) << note << '\n';
    if (parser.isSet("check")) {
        for (const auto& frame : recording.frames()) {
            for (const auto& note : frame.notes)
                QTextStream(stderr) << frame.path << ": " << note << '\n';
        }
        return recording.complete() ? 0 : 1;
    }
    application = std::make_unique<QApplication>(argc, argv);
    Player player(std::move(recording));
    player.show();
    return QCoreApplication::exec();
}

} // namespace dart::tools::video_player

#endif // DART_TOOLS_VIDEO_PLAYER_PLAYER_CPP
