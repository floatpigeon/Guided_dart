#ifndef DART_TOOLS_VIDEO_PLAYER_IMAGE_READER_CPP
#define DART_TOOLS_VIDEO_PLAYER_IMAGE_READER_CPP

#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QString>

#include "recording_types.cpp"
#include "binary_stream.cpp"

namespace dart::tools::video_player {

inline QImage read_frame_image(const Frame& frame, QString& error) {
    error.clear();
    if (frame.binary_key_offset >= 0) {
        static thread_local BinaryImageReader binary;
        return binary.read(frame, error);
    }
    if (frame.file_bytes == 0) {
        QImageReader reader(frame.path);
        const auto image = reader.read();
        error = image.isNull() ? reader.errorString() : QString{};
        return image;
    }
    QFile file(frame.path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(frame.file_offset)) {
        error = file.errorString();
        return {};
    }
    // Only the requested frame enters memory, never the whole recording.
    QByteArray data = file.read(frame.file_bytes);
    if (data.size() != frame.file_bytes) {
        error = QStringLiteral("Incomplete image record");
        return {};
    }
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto image = reader.read();
    error = image.isNull() ? reader.errorString() : QString{};
    return image;
}

} // namespace dart::tools::video_player

#endif // DART_TOOLS_VIDEO_PLAYER_IMAGE_READER_CPP
