#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "core/common/result.h"
#include "core/security/crypto.h"

struct AVFormatContext;

namespace trace {

/// Lets FFmpeg read a TRACE encrypted container without a plaintext copy
/// existing anywhere.
///
/// FFmpeg normally opens a file by path and does its own IO. That is no use for
/// encrypted evidence: the obvious alternative — decrypt to a temporary file,
/// point FFmpeg at it, delete it afterwards — writes the whole recording to disk
/// in the clear, which is the exact thing encryption at rest is for. A machine
/// that loses power mid-playback would leave that copy behind.
///
/// So FFmpeg reads through a custom `AVIOContext` instead. Its read and seek
/// callbacks go to `crypto::EncryptedFileReader`, which decrypts only the chunks
/// asked for, and the plaintext exists only in the buffer FFmpeg is holding at
/// the time.
///
/// ### Ownership
///
/// An `AVIOContext` attached to a format context with custom IO is *not* freed
/// by `avformat_close_input` — that is FFmpeg's contract, since it did not
/// allocate it. So this object owns the context, its buffer and the reader, and
/// must outlive the `AVFormatContext` it was attached to. Declare it before the
/// format context in any struct that holds both, and close the format context
/// first.
///
/// A plain, unencrypted file needs none of this: `prepare()` leaves the format
/// context alone and returns the real path for FFmpeg to open itself.
class EncryptedMediaIo {
public:
    EncryptedMediaIo();
    ~EncryptedMediaIo();
    EncryptedMediaIo(const EncryptedMediaIo&) = delete;
    EncryptedMediaIo& operator=(const EncryptedMediaIo&) = delete;

    /// Readies `*format` for `avformat_open_input` and reports the URL to pass
    /// it in `url`.
    ///
    /// - Plain file, or no key: `*format` is left null and `url` is the path.
    /// - Encrypted container with a key: `*format` is allocated with a
    ///   decrypting `pb` attached, and `url` is empty, which is how FFmpeg is
    ///   told the IO is already provided.
    ///
    /// An encrypted container with no key is refused here rather than handed to
    /// FFmpeg, which would report it as an unrecognised format and send the
    /// operator looking for a codec problem that does not exist.
    Status prepare(AVFormatContext** format, const std::filesystem::path& file,
                   const crypto::SecretKey* key, std::string* url);

    /// True when this file is being read through the decrypting path.
    bool decrypting() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace trace
