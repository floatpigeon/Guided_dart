#ifndef DART_TOOLS_VIDEO_PLAYER_RECORDING_CPP
#define DART_TOOLS_VIDEO_PLAYER_RECORDING_CPP

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace dart::tools::video_player {

struct CsvRow {
    QMap<QString, QString> values;
    QString path;
    QString raw;
    int line = 0;

    QString value(const QString& name) const { return values.value(name); }
    QString location() const { return QStringLiteral("%1:%2").arg(path).arg(line); }
    QString text() const {
        QString result = location() + '\n' + raw + '\n';
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            result += QStringLiteral("%1: %2\n").arg(it.key(), it.value());
        return result;
    }
};

// The recorder's image_index is an output counter, never a CSV line or source sequence.
struct Frame {
    quint64 image_index = 0;
    QString path;
    std::optional<CsvRow> event;
    std::optional<CsvRow> detection;
    QStringList notes;
};

inline bool split_csv(const QString& line, QStringList& fields) {
    QString field;
    bool quoted = false;
    bool closed = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c != '"') {
                field += c;
            } else if (i + 1 < line.size() && line.at(i + 1) == '"') {
                field += '"';
                ++i;
            } else {
                quoted = false;
                closed = true;
            }
        } else if (c == ',') {
            fields.push_back(field);
            field.clear();
            closed = false;
        } else if (c == '"') {
            if (!field.isEmpty() || closed)
                return false;
            quoted = true;
        } else {
            if (closed)
                return false;
            field += c;
        }
    }
    fields.push_back(field);
    return !quoted;
}

inline bool read_csv(
    const QString& path, const QStringList& required, std::vector<CsvRow>& rows,
    QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    QTextStream input(&file);
    QStringList headers;
    int line_number = 0;
    while (!input.atEnd()) {
        const QString line = input.readLine();
        ++line_number;
        if (line.trimmed().isEmpty() || line.startsWith('#'))
            continue;
        QStringList fields;
        if (!split_csv(line, fields)) {
            error = QStringLiteral("%1:%2: malformed CSV quoting").arg(path).arg(line_number);
            return false;
        }
        if (headers.isEmpty()) {
            for (const QString& name : fields) {
                if (name.isEmpty() || headers.contains(name)) {
                    error = QStringLiteral("%1:%2: empty/duplicate column").arg(path)
                                .arg(line_number);
                    return false;
                }
                headers.push_back(name);
            }
            for (const QString& name : required) {
                if (!headers.contains(name)) {
                    error = QStringLiteral("%1:%2: missing column %3 (v2 CSV required)")
                                .arg(path).arg(line_number).arg(name);
                    return false;
                }
            }
            continue;
        }
        // DetectionCsv::row(nullptr) currently emits a short empty payload. Its first
        // five fields and final admission are still unambiguous. Preserve the raw line.
        if (fields.size() < headers.size() && fields.size() > 5
            && headers.value(3) == "kind" && headers.value(4) == "valid"
            && headers.last() == "admission" && fields.at(3) == "none"
            && fields.at(4) == "0"
            && std::all_of(fields.cbegin() + 5, fields.cend() - 1,
                           [](const QString& field) { return field.isEmpty(); })) {
            const QString admission = fields.takeLast();
            while (fields.size() < headers.size() - 1)
                fields.push_back({});
            fields.push_back(admission);
        }
        if (fields.size() != headers.size()) {
            error = QStringLiteral("%1:%2: expected %3 fields, got %4")
                        .arg(path).arg(line_number).arg(headers.size()).arg(fields.size());
            return false;
        }
        CsvRow row;
        row.path = path;
        row.line = line_number;
        row.raw = line;
        for (int i = 0; i < headers.size(); ++i)
            row.values.insert(headers.at(i), fields.at(i));
        rows.push_back(std::move(row));
    }
    if (headers.isEmpty() || input.status() != QTextStream::Ok) {
        error = path + QStringLiteral(": empty CSV or read error");
        return false;
    }
    return true;
}

inline std::optional<quint64> unsigned_value(const QString& text) {
    if (text.isEmpty() || std::any_of(text.cbegin(), text.cend(), [](QChar c) {
            return c < '0' || c > '9';
        }))
        return std::nullopt;
    bool ok = false;
    const quint64 number = text.toULongLong(&ok);
    return ok ? std::optional<quint64>{number} : std::nullopt;
}

inline std::optional<QString> frame_identity(const CsvRow& row, const QString& sequence) {
    const auto seq = unsigned_value(row.value(sequence));
    const auto received = unsigned_value(row.value("received_us"));
    const auto pts = unsigned_value(row.value("source_pts"));
    if (!seq || !received || !pts)
        return std::nullopt;
    return QStringLiteral("%1/%2/%3").arg(*seq).arg(*received).arg(*pts);
}

class Recording final {
public:
    bool load(
        const QString& image_directory, const QString& frames_csv, const QString& detection_csv,
        const QString& extension, QString& error);
    const std::vector<Frame>& frames() const { return frames_; }
    const QStringList& notes() const { return notes_; }
    QString summary() const {
        return QStringLiteral("images=%1 recorder_matches=%2 detection_matches=%3 "
                              "missing_images=%4")
            .arg(frames_.size()).arg(event_matches_).arg(detection_matches_).arg(missing_images_);
    }
    bool complete() const {
        return event_matches_ == frames_.size() && missing_images_ == 0
            && (!has_detection_ || detection_matches_ == event_matches_);
    }

private:
    std::vector<Frame> frames_;
    QStringList notes_;
    std::size_t event_matches_ = 0;
    std::size_t detection_matches_ = 0;
    std::size_t missing_images_ = 0;
    bool has_detection_ = false;
};

inline bool Recording::load(
    const QString& image_directory, const QString& frames_csv, const QString& detection_csv,
    const QString& extension, QString& error) {
    *this = Recording{};
    error.clear();
    const QDir image_dir(image_directory);
    if (!image_dir.exists()) {
        error = image_directory + QStringLiteral(": image directory does not exist");
        return false;
    }
    const QRegularExpression filename(QStringLiteral("^frame_([0-9]+)\\.(pgm|pbm)$"));
    for (const QFileInfo& file : image_dir.entryInfoList(QDir::Files, QDir::Name)) {
        const auto match = filename.match(file.fileName());
        const auto index = unsigned_value(match.captured(1));
        if (!match.hasMatch() || !index || *index == 0
            || (!extension.isEmpty() && file.suffix() != extension))
            continue;
        Frame frame;
        frame.image_index = *index;
        frame.path = file.absoluteFilePath();
        frames_.push_back(std::move(frame));
    }
    std::sort(frames_.begin(), frames_.end(), [](const Frame& a, const Frame& b) {
        return a.image_index != b.image_index ? a.image_index < b.image_index : a.path < b.path;
    });
    if (frames_.empty()) {
        error = image_directory + QStringLiteral(": no frame_*.pgm/pbm images found");
        return false;
    }

    std::vector<CsvRow> events;
    if (!QFileInfo::exists(frames_csv)) {
        notes_ << frames_csv + QStringLiteral(": missing; images have no recorder metadata");
    } else if (!read_csv(frames_csv,
                        {"input_index", "source_sequence", "source_pts", "received_us",
                         "decision", "image_index", "image_file"}, events, error)) {
        return false;
    }
    std::map<QString, std::vector<const CsvRow*>> by_image;
    const QRegularExpression image_name(QStringLiteral("^img/frame_[0-9]+\\.(pgm|pbm)$"));
    for (const CsvRow& row : events) {
        if (!frame_identity(row, "source_sequence")) {
            error = row.location() + QStringLiteral(": invalid frame identity");
            return false;
        }
        const QString name = row.value("image_file");
        if (name.isEmpty())
            continue; // Downsampled/busy inputs are not pictures.
        if (!image_name.match(name).hasMatch()) {
            error = row.location() + QStringLiteral(": invalid image_file: ") + name;
            return false;
        }
        by_image[name].push_back(&row);
    }
    for (const auto& entry : by_image) {
        if (!extension.isEmpty() && !entry.first.endsWith('.' + extension))
            continue;
        if (!QFileInfo::exists(image_dir.filePath(QFileInfo(entry.first).fileName())))
            ++missing_images_;
    }

    std::vector<CsvRow> detections;
    has_detection_ = !detection_csv.isEmpty() && QFileInfo::exists(detection_csv);
    if (!has_detection_) {
        notes_ << QStringLiteral("No detection CSV; target/scan overlays unavailable.");
    } else if (!read_csv(detection_csv,
                        {"sequence", "received_us", "source_pts", "kind", "valid", "cx",
                         "cy", "win_x0", "win_y0", "win_x1", "win_y1", "admission"},
                        detections, error)) {
        return false;
    }
    std::map<QString, std::vector<const CsvRow*>> by_identity;
    for (const CsvRow& row : detections) {
        const auto identity = frame_identity(row, "sequence");
        if (!identity) {
            error = row.location() + QStringLiteral(": invalid frame identity");
            return false;
        }
        by_identity[*identity].push_back(&row);
    }
    for (Frame& frame : frames_) {
        const QString image_file = QStringLiteral("img/") + QFileInfo(frame.path).fileName();
        const auto event_it = by_image.find(image_file);
        if (event_it == by_image.end() || event_it->second.size() != 1) {
            const QString reason = event_it == by_image.end()
                ? "no matching image_file" : "ambiguous image_file";
            frame.notes << QStringLiteral("Recorder: %1; no detection association.")
                .arg(reason);
            continue;
        }
        const CsvRow& event = *event_it->second.front();
        const auto index = unsigned_value(event.value("image_index"));
        if (event.value("decision") != "accepted" || !index || *index != frame.image_index) {
            frame.notes << event.location() + QStringLiteral(": decision/image_index conflict");
            continue;
        }
        frame.event = event;
        ++event_matches_;
        if (!has_detection_)
            continue;
        const auto it = by_identity.find(*frame_identity(event, "source_sequence"));
        if (it == by_identity.end() || it->second.size() != 1) {
            frame.notes << QStringLiteral("Detection: %1 for sequence + received_us + source_pts.")
                .arg(it == by_identity.end() ? "no exact match" : "ambiguous rows");
        } else if (it->second.front()->value("admission") != "accepted") {
            frame.notes << it->second.front()->location()
                + QStringLiteral(": detection admission conflicts with recorder");
        } else {
            frame.detection = *it->second.front();
            ++detection_matches_;
        }
    }
    if (missing_images_ != 0)
        notes_ << QStringLiteral("%1 CSV image_file entries have no image on disk.")
            .arg(missing_images_);
    return true;
}

} // namespace dart::tools::video_player

#endif // DART_TOOLS_VIDEO_PLAYER_RECORDING_CPP
