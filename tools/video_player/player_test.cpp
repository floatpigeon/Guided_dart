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
        test.empty_csv();
        test.gui();
        streams();
        png_frames();
        flattened_recording();
        session_collection();
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
    static void session_collection() {
        QTemporaryDir directory{QDir::tempPath() + "/dart recording collection-XXXXXX"};
        require(directory.isValid(), "recording collection directory");
        const QString root = directory.path();
        const QString first = root + "/rec_2", second = root + "/rec_10";
        const QString image = "img/b0000001/p0000001/00000001.png";
        const QString header =
            "input_index,source_sequence,source_pts,source_pts_timebase_validated,received_us,"
            "decision,image_index,image_file,file_offset,file_bytes\n";
        QImage png(9, 2, QImage::Format_Grayscale8);
        png.fill(0);
        for (const auto& session : {second, first}) {
            require(
                QDir(session).mkpath("img/b0000001/p0000001") && QDir(session).mkpath("logs"),
                "collection session directories");
            require(png.save(session + '/' + image), "collection PNG");
            const QString identity = session == first ? "101,1000,0,100000" : "101,2000,0,200000";
            write_text(
                session + "/logs/frames.csv",
                header + QStringLiteral("1,%1,accepted,1,%2,0,%3\n")
                             .arg(identity, image)
                             .arg(QFileInfo(session + '/' + image).size()));
        }
        require(QDir(root).mkpath("unrelated"), "unrelated collection subdirectory");
        write_text(
            root + "/detection.csv", detection_header() + detection_row("101", "100000", "1000")
                                         + detection_row("101", "200000", "2000", false));
        Recording recording;
        QString error;
        require(
            recording.load_directory(root, std::nullopt, std::nullopt, {}, error),
            "load session collection");
        require(
            recording.complete() && recording.frames().size() == 2
                && recording.summary().contains("detection_matches=2"),
            "collection keeps both repeated image indexes and filenames");
        require(
            recording.frames()[0].path.startsWith(first + '/')
                && recording.frames()[1].path.startsWith(second + '/')
                && recording.frames()[0].event->path == first + "/logs/frames.csv"
                && recording.frames()[1].event->path == second + "/logs/frames.csv"
                && recording.frames()[0].detection->value("source_pts") == "1000"
                && recording.frames()[1].detection->value("source_pts") == "2000",
            "sessions sort numerically and associate shared detections using their own recorder CSV");
        require(
            cli({"--check", "--png", root}, 0).contains("images=2 recorder_matches=2 detection_matches=2"),
            "CLI accepts collection root");
        require(
            cli({"--check", root, first + "/logs/frames.csv"}, 1).contains("Multiple sessions"),
            "one explicit recorder CSV cannot be reused across sessions");
        cli({"--check", root, "", ""}, 1);
        Player player(recording);
        auto* slider = player.findChild<QSlider*>("frame_slider");
        auto* info = player.findChild<QPlainTextEdit*>("frame_info");
        require(slider && info && slider->maximum() == 1, "collection GUI frame range");
        slider->setValue(1);
        require(
            info->toPlainText().contains(second + '/' + image)
                && info->toPlainText().contains("source_pts: 2000")
                && !info->toPlainText().contains("IMAGE ERROR"),
            "GUI steps across the session boundary with matching image and metadata");

        // Metadata availability is scoped per session, including repeated frame identities.
        require(QFile::remove(root + "/detection.csv"), "remove shared detection CSV");
        write_text(first + "/detection.csv", detection_header() + detection_row("101", "100000", "1000"));
        require(
            cli({"--check", root}, 0).contains("detection_matches=1"),
            "one record-only session does not invalidate another session's detections");
        write_text(
            second + "/logs/frames.csv",
            header + QStringLiteral("1,101,1000,0,100000,accepted,1,%1,0,%2\n")
                         .arg(image)
                         .arg(QFileInfo(second + '/' + image).size()));
        write_text(second + "/detection.csv", detection_header() + detection_row("101", "100000", "1000", false));
        require(
            recording.load_directory(root, std::nullopt, std::nullopt, {}, error)
                && recording.complete()
                && recording.frames()[0].detection->value("valid") == "1"
                && recording.frames()[1].detection->value("valid") == "0",
            "local detection CSVs cannot cross-associate identical identities in separate sessions");
        write(second + "/logs/frames.csv", {});
        cli({"--check", root}, 1);

        // A flat recording at the root remains a single recording.
        require(png.save(root + "/00000001.png"), "flat image beside child sessions");
        require(
            recording.load_directory(root, std::nullopt, std::nullopt, {}, error)
                && recording.frames().size() == 1
                && recording.frames()[0].path == root + "/00000001.png",
            "flat image directory takes precedence over child sessions");
    }
    static void flattened_recording() {
        QTemporaryDir directory{QDir::tempPath() + "/dart flat recording-XXXXXX"};
        require(directory.isValid(), "flattened recording directory");
        const QString root = directory.path() + "/records";
        require(
            QDir(root).mkpath("b0000001/p0000001")
                && QDir(root).mkpath("b0000001/p0000002")
                && QDir(directory.path()).mkpath("logs"),
            "flattened recording groups");
        // Unrelated parent logs must not be selected for a flattened archive.
        write(directory.path() + "/logs/frames.csv", "unrelated recorder CSV\n");
        write(directory.path() + "/detection.csv", "unrelated detection CSV\n");
        const QString a = "b0000001/p0000001/00000032.png";
        const QString b = "b0000001/p0000002/00000033.png";
        const QString c = "b0000001/p0000002/00000034.png";
        QImage image(640, 480, QImage::Format_Mono);
        image.setColorTable({qRgb(0, 0, 0), qRgb(255, 255, 255)});
        image.fill(0);
        image.setPixel(639, 479, 1);
        for (const auto& name : {a, b, c})
            require(image.save(root + '/' + name), "save monochrome PNG");
        const QString header =
            "# recorder format=2 width=640 height=480 fps=60 session=/data/app/records/rec_0001\n"
            "input_index,source_sequence,source_pts,source_pts_timebase_validated,received_us,"
            "decision,image_index,image_file,file_offset,file_bytes\n";
        // Physical CSV order, input sequence and output image index are independent.
        const QString rows = QStringLiteral(
                                 "2,102,1001,0,110000,downsampled,0,,0,0\n"
                                 "4,202,2001,0,210000,busy,0,,0,0\n"
                                 "3,201,2000,0,200000,accepted,33,img/%1,0,%2\n"
                                 "1,101,1000,0,100000,accepted,32,img/%3,0,%4\n")
                                 .arg(b)
                                 .arg(QFileInfo(root + '/' + b).size())
                                 .arg(a)
                                 .arg(QFileInfo(root + '/' + a).size());
        write_text(root + "/frames.csv", header + rows);
        const auto paths = RecordingPaths::resolve(root);
        require(
            paths.images == root && paths.frames_csv == root + "/frames.csv"
                && paths.detection_csv == root + "/detection.csv",
            "flattened archive resolves local metadata");
        Recording recording;
        QString error;
        require(
            recording.load(paths.images, paths.frames_csv, paths.detection_csv, {}, error),
            "load flattened recording");
        require(
            recording.frames().size() == 3 && !recording.complete()
                && recording.summary().contains("recorder_matches=2")
                && recording.summary().contains("missing_images=0"),
            "unlogged tail remains visible and incomplete");
        const auto& first = recording.frames()[0];
        const auto& second = recording.frames()[1];
        require(
            first.image_index == 32 && first.event && first.event->line == 6
                && first.event->value("source_sequence") == "101" && second.event
                && second.event->line == 5 && second.event->value("source_sequence") == "201",
            "flattened groups match original img-prefixed paths in unordered CSV");
        const QImage decoded = read_frame_image(first, error);
        require(
            decoded.size() == QSize(640, 480) && qRed(decoded.pixel(0, 0)) == 0
                && qRed(decoded.pixel(639, 479)) == 255,
            "one-bit PNG dimensions and polarity");
        require(
            !recording.frames()[2].event && !recording.frames()[2].notes.isEmpty(),
            "unlogged image cannot borrow adjacent metadata");
        const QString output = cli({"--check", root}, 1);
        require(output.contains("recorder_matches=2"), "CLI discovers local frames.csv");
        Player player(recording);
        auto* slider = player.findChild<QSlider*>("frame_slider");
        auto* info = player.findChild<QPlainTextEdit*>("frame_info");
        require(slider && info, "flattened recording controls");
        slider->setValue(1);
        require(info->toPlainText().contains("source_sequence: 201"), "flat frame log in GUI");
        slider->setValue(2);
        require(
            info->toPlainText().contains("no matching image_file")
                && !info->toPlainText().contains("source_sequence:"),
            "flat unlogged frame has its own diagnostic");
        write(root + '/' + c, {}); // Interrupted recording may leave an empty final PNG.
        slider->setValue(0);
        slider->setValue(2);
        require(
            info->toPlainText().contains("IMAGE ERROR")
                && !info->toPlainText().contains("source_sequence:"),
            "empty unlogged PNG reports an image error without borrowed metadata");
        require(image.save(root + '/' + c), "restore tail PNG");
        const QString tail = QStringLiteral("5,301,3000,0,300000,accepted,34,img/%1,0,%2\n")
                                 .arg(c)
                                 .arg(QFileInfo(root + '/' + c).size());
        write_text(root + "/frames.csv", header + rows + tail);
        cli({"--check", "--png", root}, 0);
        cli({"--check", root, root + "/frames.csv"}, 0);
        write_text(
            root + "/detection.csv",
            detection_header() + detections() + detection_row("301", "300000", "3000"));
        require(
            cli({"--check", root}, 0).contains("detection_matches=3"),
            "flattened archive discovers local detection CSV");
    }
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
        write_text(frames, csv_header + rows);
        cli({"--check", root}, 0);
        cli({"--check", root + "/img"}, 0);
        require(QDir(root).mkpath("rec_0001"), "create original session layout");
        require(
            QDir(root).rename("img", "rec_0001/img")
                && QDir(root).rename("logs", "rec_0001/logs"),
            "move fixture into original session layout");
        require(
            cli({"--check", root + "/rec_0001"}, 0).contains("detection_matches=2"),
            "original session discovers parent detection CSV");
        cli({"--check", root + "/rec_0001/img"}, 0);
        require(
            QFile::rename(root + "/rec_0001/logs/frames.csv", root + "/rec_0001/frames.csv"),
            "move CSV to session root");
        cli({"--check", root + "/rec_0001"}, 0);
        cli({"--check", root + "/rec_0001/img"}, 0);
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
    static QString cli(const QStringList& args, int expected) {
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "intentionally_unavailable");
        process.setProcessEnvironment(env);
        process.start(QCoreApplication::applicationDirPath() + "/video_player", args);
        require(process.waitForFinished(10000), "CLI finished without a display server");
        require(process.exitStatus() == QProcess::NormalExit, "CLI normal exit");
        const QString output =
            QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
        if (process.exitCode() != expected)
            throw std::runtime_error(output.toStdString());
        return output;
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
        require(
            cli({"--check", root()}, 0).contains("detection_matches=2"),
            "session discovers its archived detection CSV");
        cli({"--check", root() + "/img"}, 0);
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
    void empty_csv() {
        const QString whitespace = "\n \t\r\n  # metadata pending\n\n";
        for (const auto& content : {QString{}, whitespace, detection_header()}) {
            write_text(detection_path(), content);
            const auto recording = load();
            require(
                recording.complete() && recording.frames().front().event
                    && !recording.frames().front().detection
                    && recording.notes().join('\n').contains("no data rows"),
                "empty detection CSV permits recorder-only viewing and checks");
            const auto overlay = ImageView::overlay_for(recording.frames().front(), QSize(9, 2));
            require(!overlay.center && !overlay.window, "empty detection CSV has no overlay");
            cli({"--check", root()}, 0);
            cli({"--check", root(), frames_path(), detection_path()}, 0);
        }
        reset();
        for (const auto& content : {QString{}, whitespace, frames_header()}) {
            write_text(frames_path(), content);
            const auto recording = load();
            require(
                recording.frames().size() == 2 && !recording.complete()
                    && !recording.frames().front().event && !recording.frames().front().detection
                    && recording.notes().join('\n').contains("no data rows"),
                "empty recorder CSV permits viewing without borrowing detection metadata");
            cli({"--check", root(), frames_path(), detection_path()}, 1);
        }
        write(detection_path(), {});
        const auto recording = load();
        Player player(recording);
        auto* slider = player.findChild<QSlider*>("frame_slider");
        auto* info = player.findChild<QPlainTextEdit*>("frame_info");
        require(slider && info, "image-only player controls");
        slider->setValue(1);
        require(
            info->toPlainText().contains("frame_00000003.pbm")
                && info->toPlainText().contains("no data rows")
                && !info->toPlainText().contains("IMAGE ERROR")
                && !info->toPlainText().contains("source_sequence:"),
            "both empty CSVs permit image-only GUI stepping");
        cli({"--check", root()}, 1);
        reset();
        require(
            cli({"--check", root(), frames_path(), ""}, 0).contains("detection_matches=0"),
            "empty detection argument disables default detection CSV");
        require(
            cli({"--check", root(), "", ""}, 1).contains("recorder_matches=0"),
            "empty CSV arguments disable default metadata");
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
