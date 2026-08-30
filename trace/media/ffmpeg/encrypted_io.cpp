#include "media/ffmpeg/encrypted_io.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/mem.h>
}

#include "core/common/logging.h"

namespace trace {
namespace {

constexpr const char* kComponent = "media";

/// FFmpeg's own default for file IO. Large enough that the decrypting reader is
/// asked for whole chunks rather than a few bytes at a time.
constexpr int kIoBufferBytes = 32 * 1024;

}  // namespace

struct EncryptedMediaIo::Impl {
    std::unique_ptr<crypto::EncryptedFileReader> reader;
    AVIOContext* io = nullptr;
    std::uint64_t position = 0;
    bool attached = false;

    /// FFmpeg asks for bytes at the current position. Returning AVERROR_EOF at
    /// the end rather than 0 is what tells it the stream finished normally; a
    /// failed authentication is a hard error, and must never be reported as a
    /// short read, because a short read looks like a shorter recording.
    static int readPacket(void* opaque, std::uint8_t* into, int wanted) {
        auto* self = static_cast<Impl*>(opaque);
        if (wanted <= 0) return 0;
        if (self->position >= self->reader->size()) return AVERROR_EOF;

        auto got = self->reader->read(self->position, into, static_cast<std::size_t>(wanted));
        if (!got) {
            logError(kComponent, "Encrypted evidence failed to decrypt during playback",
                     JsonValue::object().set("detail", got.error().toString()));
            return AVERROR_INVALIDDATA;
        }
        const std::size_t bytes = got.take();
        if (bytes == 0) return AVERROR_EOF;
        self->position += bytes;
        return static_cast<int>(bytes);
    }

    /// Seeking is the reason the container is chunked at all. AVSEEK_SIZE is
    /// FFmpeg asking for the length rather than moving; several demuxers need it
    /// to work out durations, and answering it wrong makes a file look
    /// unseekable.
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
        auto* self = static_cast<Impl*>(opaque);
        const std::int64_t size = static_cast<std::int64_t>(self->reader->size());

        if ((whence & AVSEEK_SIZE) != 0) return size;
        whence &= ~AVSEEK_FORCE;

        std::int64_t target = 0;
        switch (whence) {
            case SEEK_SET: target = offset; break;
            case SEEK_CUR: target = static_cast<std::int64_t>(self->position) + offset; break;
            case SEEK_END: target = size + offset; break;
            default: return AVERROR(EINVAL);
        }
        if (target < 0) return AVERROR(EINVAL);
        // Seeking to or past the end is legal; the next read reports EOF.
        self->position = static_cast<std::uint64_t>(target);
        return target;
    }

    ~Impl() {
        if (io != nullptr) {
            // The buffer may have been reallocated by FFmpeg, so free the one it
            // is holding now rather than the one that was handed over.
            if (io->buffer != nullptr) av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};

EncryptedMediaIo::EncryptedMediaIo() : impl_(std::make_unique<Impl>()) {}
EncryptedMediaIo::~EncryptedMediaIo() = default;

bool EncryptedMediaIo::decrypting() const { return impl_->attached; }

Status EncryptedMediaIo::prepare(AVFormatContext** format, const std::filesystem::path& file,
                                 const crypto::SecretKey* key, std::string* url) {
    *url = file.string();

    if (!crypto::looksEncrypted(file)) {
        // An ordinary file. Nothing to do: FFmpeg opens it itself, exactly as
        // before encryption existed.
        return Status::success();
    }
    if (key == nullptr) {
        return Status::failure(
            ErrorCode::PermissionDenied, "This evidence is encrypted and the workspace is locked",
            "Sign in to the workspace that holds this case before opening its evidence.");
    }

    impl_->reader = std::make_unique<crypto::EncryptedFileReader>();
    if (auto status = impl_->reader->open(file, *key); !status) {
        impl_->reader.reset();
        return status;
    }

    auto* buffer = static_cast<std::uint8_t*>(av_malloc(kIoBufferBytes));
    if (buffer == nullptr) {
        impl_->reader.reset();
        return Status::failure(ErrorCode::Internal, "Unable to allocate a media read buffer");
    }
    impl_->io = avio_alloc_context(buffer, kIoBufferBytes, /*write_flag=*/0, impl_.get(),
                                   &Impl::readPacket, /*write_packet=*/nullptr, &Impl::seek);
    if (impl_->io == nullptr) {
        av_free(buffer);
        impl_->reader.reset();
        return Status::failure(ErrorCode::Internal, "Unable to create a media IO context");
    }
    // Without this, FFmpeg assumes a stream it cannot seek and falls back to
    // reading forward from the start for every seek — which for an hour of
    // footage is the difference between instant and unusable.
    impl_->io->seekable = AVIO_SEEKABLE_NORMAL;

    *format = avformat_alloc_context();
    if (*format == nullptr) {
        return Status::failure(ErrorCode::Internal, "Unable to allocate a media format context");
    }
    (*format)->pb = impl_->io;
    // Set explicitly rather than relying on avformat_open_input to infer it:
    // this flag is also what stops avformat_close_input from freeing a context
    // it does not own.
    (*format)->flags |= AVFMT_FLAG_CUSTOM_IO;

    // An empty URL is how FFmpeg is told the IO is already provided. Passing the
    // real path here would make it open the file a second time, in the clear.
    url->clear();
    impl_->attached = true;
    return Status::success();
}

}  // namespace trace
