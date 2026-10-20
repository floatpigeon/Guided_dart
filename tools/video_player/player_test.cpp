#include <QApplication>
#include <QFile>
#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSlider>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>

#include "player.cpp"

namespace dart::tools::video_player::tests {

inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class PlayerTest final {
public:
    static void run() {
        PlayerTest test;
        require(test.directory_.isValid(), "temporary directory");
        require(QDir(test.root()).mkpath("img"), "create img");
        require(QDir(test.root()).mkpath("logs"), "create logs");
        test.reset();
        test.correspondence();
        test.conflicts();
        test.invalid_csv();
        test.gui();
        streams();
        png_frames();
        std::puts("video player: association, CSV, PGM/PBM, overlays, GUI and CLI passed");
    }

private:
    QTemporaryDir directory_{QDir::tempPath() + "/dart player-XXXXXX"};
    QString root() const { return directory_.path(); }
    QString frames_path() const { return root() + "/logs/frames.csv"; }
    QString detection_path() const { return root() + "/detection.csv"; }
    static QString frames_header() {
        return "# recorder format=1 width=9 height=2 fps=30 session=/sdcard/records/rec_0001\n"
               "input_index,source_sequence,source_pts,source_pts_timebase_validated,received_us,"
               "decision,image_index,image_file\n";
    }
    static QString events() {
        return "1,101,1000,0,100000,accepted,1,img/frame_00000001.pgm\n"
               "2,102,1001,0,110000,downsampled,0,\n"
               "3,103,1002,0,120000,busy,0,\n"
               "4,201,2000,0,200000,accepted,3,img/frame_00000003.pbm\n";
    }
    static QString detection_header() {
        return "sequence,received_us,source_pts,kind,valid,cx,cy,area,radius,quality,clipped,"
               "missed_frames,candidates,full_frame,rescanned,win_x0,win_y0,win_x1,win_y1,"
               "admission\n";
    }
    static QString detection_row(
        const QString& seq, const QString& received, const QString& pts, bool valid = true) {
        return QStringLiteral("%1,%2,%3,light,%4,2.5,0.5,4,1.1,0.8,0,0,1,1,0,0,0,9,2,accepted\n")
            .arg(seq, received, pts, valid ? "1" : "0");
    }
    static QString detections() {
        return detection_row("201", "200000", "2000", false)
             + detection_row("101", "99999", "1000") // Same sequence in another run.
             + detection_row("101", "100000", "999") // Same time but wrong PTS.
             + detection_row("101", "100000", "1000");
    }
    static void write(const QString& path, const QByteArray& bytes) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "open fixture");
        require(file.write(bytes) == bytes.size(), "write fixture");
    }
    static void write_text(const QString& path, const QString& text) { write(path, text.toUtf8()); }
    static void png_frames() {
        QTemporaryDir directory;
        require(directory.isValid(), "PNG directory");
        const QString root = directory.path();
        require(
            QDir(root).mkpath("img/b0000001/p0000032") && QDir(root).mkpath("img/b0000002/p0000001")
                && QDir(root).mkpath("logs"),
            "PNG subdirectories");
        QImage first(9, 2, QImage::Format_Grayscale8);
        first.fill(0);
        first.setPixel(8, 1, qRgb(255, 255, 255));
        const QString a = "img/b0000001/p0000032/00001024.png";
        const QString b = "img/b0000002/p0000001/00001025.png";
        require(first.save(root + '/' + a), "save first PNG");
        QImage second = first;
        second.invertPixels();
        require(second.save(root + '/' + b), "save second PNG");
        write(root + "/img/b0000002/p0000001/frame_00001026.tmp", "partial image");
        const QString csv_header =
            "input_index,source_sequence,source_pts,source_pts_timebase_validated,received_us,"
            "decision,image_index,image_file,file_offset,file_bytes\n";
        const QString rows = QStringLiteral(
                                 "1,101,1000,0,100000,accepted,1024,%1,0,%2\n"
                                 "2,201,2000,0,200000,accepted,1025,%3,0,%4\n")
                                 .arg(a)
                                 .arg(QFileInfo(root + '/' + a).size())
                                 .arg(b)
                                 .arg(QFileInfo(root + '/' + b).size());
        const QString frames = root + "/logs/frames.csv", detection = root + "/detection.csv";
        write_text(frames, csv_header + rows);
        write_text(detection, detection_header() + detections());
        Recording recording;
        QString error;
        require(recording.load(root + "/img", frames, detection, "png", error), "load PNG groups");
        require(
            recording.complete() && recording.frames().size() == 2, "PNG metadata associations");
        require(
            recording.frames()[0].image_index == 1024 && recording.frames()[1].image_index == 1025,
            "PNG ordering across directories");
        require(read_frame_image(recording.frames()[0], error) == first, "decode first PNG");
        require(read_frame_image(recording.frames()[1], error) == second, "decode second PNG");
        Player player(recording);
        auto* slider = player.findChild<QSlider*>("frame_slider");
        require(slider != nullptr, "PNG slider");
        slider->setValue(1);
        auto* info = player.findChild<QPlainTextEdit*>("frame_info");
        require(
            info && info->toPlainText().contains("source_sequence: 201")
                && !info->toPlainText().contains("IMAGE ERROR"),
            "PNG GUI matches metadata");
        const auto first_bytes = QFileInfo(root + '/' + a).size();
        const QString original = QStringLiteral("%1,0,%2\n").arg(a).arg(first_bytes);
        for (const auto& conflict :
             {QStringLiteral("%1,1,%2\n").arg(a).arg(first_bytes),
              QStringLiteral("%1,0,%2\n").arg(a).arg(first_bytes + 1)}) {
            QString bad_rows = rows;
            bad_rows.replace(original, conflict);
            write_text(frames, csv_header + bad_rows);
            require(
                recording.load(root + "/img", frames, detection, "png", error),
                "load PNG metadata conflict");
            require(
                !recording.complete() && !recording.frames()[0].event
                    && !recording.frames()[0].detection && recording.frames()[1].detection,
                "PNG offset/size conflict must disable only the mismatched association");
        }
    }
    static void streams() {
        for (const QString& extension : {QString("pgm"), QString("pbm")}) {
            QTemporaryDir directory;
            require(directory.isValid(), "stream directory");
            const QString root = directory.path();
            require(QDir(root).mkpath("img") && QDir(root).mkpath("logs"), "stream subdirectories");
            const bool pbm = extension == "pbm";
            const QByteArray header = pbm ? "P4\n9 2\n" : "P5\n9 2\n255\n";
            const QByteArray first =
                header
                + (pbm ? QByteArray::fromHex("80800000")
                       : QByteArray::fromHex("0a202300ff102030405060708090a0b0c0d0"));
            const QByteArray second =
                header + (pbm ? QByteArray::fromHex("00008080") : QByteArray(18, '\x55'));
            const QString path = root + "/img/recording." + extension;
            write(path, first + second + first.left(5)); // Reset during the third frame.
            const QString csv_header =
                "# recorder format=2 width=9 height=2 fps=60\n"
                "input_index,source_sequence,source_pts,source_pts_timebase_validated,received_us,"
                "decision,image_index,image_file,file_offset,file_bytes\n";
            const QString rows = QStringLiteral(
                                     "1,101,1000,0,100000,accepted,1,img/recording.%1,0,%2\n"
                                     "2,201,2000,0,200000,accepted,2,img/recording.%1,%2,%2\n")
                                     .arg(extension)
                                     .arg(first.size());
            const QString frames = root + "/logs/frames.csv";
            const QString detection = root + "/detection.csv";
            write_text(frames, csv_header + rows);
            write_text(detection, detection_header() + detections());
            Recording recording;
            QString error;
            require(
                recording.load(root + "/img", frames, detection, extension, error), "load stream");
            require(recording.complete() && recording.frames().size() == 2, "stream associations");
            require(
                recording.notes().join(' ').contains("incomplete final frame"),
                "reset tail ignored");
            require(recording.frames()[1].file_offset == first.size(), "second frame offset");
            const QImage a = read_frame_image(recording.frames()[0], error);
            require(!a.isNull() && error.isEmpty(), "decode first stream frame");
            const QImage b = read_frame_image(recording.frames()[1], error);
            require(!b.isNull() && error.isEmpty() && a != b, "decode second stream frame");
            require(qRed(a.pixel(0, 0)) == (pbm ? 0 : 10), "binary first pixel preserved");
            require(qRed(b.pixel(0, 0)) == (pbm ? 255 : 85), "seek to second frame pixels");
            Player player(recording);
            auto* slider = player.findChild<QSlider*>("frame_slider");
            require(slider != nullptr, "stream slider");
            slider->setValue(1);
            auto* info = player.findChild<QPlainTextEdit*>("frame_info");
            require(
                info && info->toPlainText().contains("source_sequence: 201")
                    && !info->toPlainText().contains("IMAGE ERROR"),
                "stream GUI frame matches log");

            write_text(
                frames,
                csv_header + rows
                    + QStringLiteral("3,301,3000,0,300000,accepted,3,img/recording.%1,%2,%3\n")
                          .arg(extension)
                          .arg(first.size() * 2)
                          .arg(first.size()));
            require(
                recording.load(root + "/img", frames, {}, {}, error) && !recording.complete()
                    && recording.summary().contains("missing_images=1"),
                "CSV cannot match an incomplete stream frame");
            write_text(frames, csv_header + rows);
            write(path, first + second.left(second.size() - 1));
            require(
                recording.load(root + "/img", frames, {}, {}, error)
                    && recording.frames().size() == 1,
                "truncated payload not decoded as frame");
        }
    }
    void reset() {
        QByteArray pgm("P5\n9 2\n255\n");
        pgm += QByteArray::fromHex("0a202300ff102030405060708090a0b0c0d0");
        write(root() + "/img/frame_00000001.pgm", pgm);
        write(
            root() + "/img/frame_00000003.pbm",
            QByteArray("P4\n9 2\n") + QByteArray::fromHex("80800000"));
        write_text(frames_path(), frames_header() + events());
        write_text(detection_path(), detection_header() + detections());
    }
    Recording load(const QString& extension = {}) const {
        Recording recording;
        QString error;
        if (!recording.load(root() + "/img", frames_path(), detection_path(), extension, error))
            throw std::runtime_error(error.toStdString());
        return recording;
    }
    void cli(const QStringList& args, int expected) const {
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "intentionally_unavailable");
        process.setProcessEnvironment(env);
        process.start(QCoreApplication::applicationDirPath() + "/video_player", args);
        require(process.waitForFinished(10000), "CLI finished without a display server");
        require(process.exitStatus() == QProcess::NormalExit, "CLI normal exit");
        if (process.exitCode() != expected) {
            throw std::runtime_error(
                (process.readAllStandardError() + process.readAllStandardOutput()).toStdString());
        }
    }
    void correspondence() {
        const auto recording = load();
        require(recording.complete() && recording.frames().size() == 2, "complete association");
        const auto& first = recording.frames().at(0);
        const auto& second = recording.frames().at(1);
        require(first.event->line == 3 && second.event->line == 6, "physical CSV source lines");
        require(first.detection->value("received_us") == "100000", "receipt identity");
        require(first.detection->value("source_pts") == "1000", "PTS identity");
        require(second.event->value("source_sequence") == "201", "gaps are not row offsets");
        require(second.detection->line == 2, "detection order independent from image order");
        const auto pgm = QImageReader(first.path).read();
        require(!pgm.isNull() && pgm.size() == QSize(9, 2), "PGM load");
        require(
            qRed(pgm.pixel(0, 0)) == 10 && qRed(pgm.pixel(1, 0)) == 32
                && qRed(pgm.pixel(2, 0)) == 35 && qRed(pgm.pixel(4, 0)) == 255,
            "PGM binary whitespace/hash are pixel data");
        const auto pbm = QImageReader(second.path).read();
        require(!pbm.isNull() && pbm.size() == QSize(9, 2), "PBM load");
        require(
            qRed(pbm.pixel(0, 0)) == 0 && qRed(pbm.pixel(8, 0)) == 0 && qRed(pbm.pixel(1, 0)) == 255
                && qRed(pbm.pixel(0, 1)) == 255,
            "PBM polarity and row padding");
        const auto overlay = ImageView::overlay_for(first, pgm.size());
        require(overlay.center == QPointF(2.5, 0.5), "fractional center retained");
        require(overlay.window == QRectF(0, 0, 9, 2), "exclusive window bounds");
        require(!ImageView::overlay_for(second, pbm.size()).center, "invalid target hidden");
        Frame bad = first;
        bad.detection->values["cx"] = "nan";
        bad.detection->values["win_x1"] = "10";
        const auto invalid = ImageView::overlay_for(bad, pgm.size());
        require(!invalid.center && !invalid.window, "nonfinite/outside overlays hidden");
        require(load("pbm").frames().size() == 1, "format filter");
        cli({"--help"}, 0);
        cli({"--help-all"}, 0);
        cli({}, 2);
        cli({"--unknown"}, 2);
        cli({"--check", root(), frames_path(), detection_path()}, 0);
        cli({"--check", root() + "/img", frames_path(), detection_path()}, 0);
        cli({"--check", root(), root() + "/missing.csv"}, 2);

        // v1 truncated filenames to six digits. Also exercise full-width integer identities.
        const QString long_image = root() + "/img/frame_10000001.pgm";
        require(QFile::copy(first.path, long_image), "large-index image fixture");
        write_text(
            frames_path(), frames_header() + events()
                               + "5,18446744073709551615,18446744073709551614,0,300000,accepted,"
                                 "10000001,img/frame_10000001.pgm\n");
        write_text(
            detection_path(),
            detection_header() + detections()
                + detection_row("18446744073709551615", "300000", "18446744073709551614"));
        const auto large = load();
        require(
            large.complete() && large.frames().back().image_index == 10000001,
            "complete numeric image suffix and uint64 identity");
        require(QFile::remove(long_image), "remove large-index fixture");
        reset();
    }
    void conflicts() {
        write_text(
            detection_path(),
            detection_header() + detections() + detection_row("101", "100000", "1000"));
        const auto duplicate = load();
        require(!duplicate.frames().at(0).detection, "duplicate identity is ambiguous");
        require(!duplicate.complete(), "ambiguous association reported");
        cli({"--check", root(), frames_path(), detection_path()}, 1);
        write_text(
            frames_path(), frames_header() + events()
                               + "5,201,2000,0,200000,accepted,3,img/frame_00000003.pbm\n"
                               + "6,301,3000,0,300000,accepted,2,img/frame_00000002.pgm\n");
        write(root() + "/img/frame_00000004.pgm", QByteArray("P5\n1 1\n255\nX"));
        const auto missing = load();
        require(!missing.frames().at(1).event, "duplicate image_file is ambiguous");
        require(!missing.frames().at(2).event, "orphan image has no borrowed metadata");
        require(missing.summary().contains("missing_images=1"), "absent image reported");
        require(QFile::remove(root() + "/img/frame_00000004.pgm"), "remove orphan fixture");
        reset();

        // Actual shortened row emitted when vision is disabled / Detect returns no result.
        write_text(
            detection_path(), detection_header() + "101,100000,1000,none,0,,,,,,,,,,,accepted\n"
                                  + detection_row("201", "200000", "2000", false));
        const auto disabled = load();
        require(disabled.complete(), "short disabled-detection row identity");
        require(
            disabled.frames().front().detection->value("admission") == "accepted",
            "short row admission remains in its own column");
        const auto empty = ImageView::overlay_for(disabled.frames().front(), QSize(9, 2));
        require(!empty.center && !empty.window, "disabled detection has no overlay");
        reset();
        require(QFile::remove(detection_path()), "remove detection fixture");
        require(load().complete(), "record-only viewing");
        require(QFile::remove(frames_path()), "remove recorder fixture");
        const auto no_metadata = load();
        require(
            !no_metadata.complete() && !no_metadata.frames().front().event,
            "missing recorder never falls back to line number");
        reset();
    }
    void invalid_csv() {
        QStringList fields;
        require(split_csv("a,\"b,c\",\"d\"\"e\",", fields), "quoted CSV");
        require(fields == QStringList({"a", "b,c", "d\"e", ""}), "CSV field content");
        for (const QString& content :
             {QString("sequence,sequence\n1,2\n"),
              detection_header() + "101,100000,1000,light,1,2\n",
              detection_header() + "\"unclosed\n"}) {
            write_text(detection_path(), content);
            Recording recording;
            QString error;
            require(
                !recording.load(root() + "/img", frames_path(), detection_path(), {}, error),
                "malformed CSV rejected");
            require(error.contains(detection_path()), "CSV diagnostic includes source file");
        }
        reset();
        write_text(
            frames_path(),
            frames_header() + "1,101,1000,0,100000,accepted,1,../frame_00000001.pgm\n");
        Recording recording;
        QString error;
        require(
            !recording.load(root() + "/img", frames_path(), detection_path(), {}, error),
            "non-session image_file rejected");
        reset();
    }
    void gui() {
        Player player(load());
        player.show();
        QApplication::processEvents();
        auto* slider = player.findChild<QSlider*>("frame_slider");
        auto* info = player.findChild<QPlainTextEdit*>("frame_info");
        auto* view = player.findChild<QWidget*>("image_view");
        require(slider && info && view, "player controls");
        player.activateWindow();
        view->setFocus();
        QApplication::processEvents();
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(view, &right);
        QApplication::processEvents();
        require(slider->value() == 1, "right arrow advances frame");
        QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
        QApplication::sendEvent(view, &home);
        QApplication::processEvents();
        require(slider->value() == 0, "Home selects first frame");
        slider->setValue(1);
        QApplication::processEvents();
        require(info->toPlainText().contains("source_sequence: 201"), "slider selects exact log");
        require(info->toPlainText().contains("frame_00000003.pbm"), "slider selects exact image");
        QMouseEvent click(
            QEvent::MouseButtonPress, QPointF(view->rect().center()), Qt::LeftButton,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &click);
        require(info->toPlainText().contains("Clicked pixel ("), "pixel click visible in log pane");
        const QString clicked = info->toPlainText();
        QMouseEvent border(
            QEvent::MouseButtonPress, QPointF(0, 0), Qt::LeftButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(view, &border);
        require(info->toPlainText() == clicked, "letterbox click ignored");
        const QString screenshot = qEnvironmentVariable("DART_PLAYER_TEST_SCREENSHOT");
        if (!screenshot.isEmpty())
            require(player.grab().save(screenshot), "save GUI verification image");
        write(root() + "/img/frame_00000003.pbm", "not an image");
        slider->setValue(0);
        slider->setValue(1);
        QApplication::processEvents();
        require(info->toPlainText().contains("IMAGE ERROR"), "damaged image reported");
        const auto screen = view->grab().toImage();
        require(
            qRed(screen.pixel(screen.width() / 2, screen.height() / 2)) == 0,
            "failed image clears previous pixels");
    }
};

} // namespace dart::tools::video_player::tests

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        dart::tools::video_player::tests::PlayerTest::run();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
