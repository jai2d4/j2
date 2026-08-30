#include "core/security/crypto.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "core/security/password.h"

#if defined(TRACE_WITH_ENCRYPTION)
#include <openssl/crypto.h>
#include <openssl/evp.h>
#endif

namespace trace::crypto {
namespace {

constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kAlgorithmAes256Gcm = 1;

/// Little-endian, fixed width, everywhere. The container is read on whichever
/// machine holds the disk, which is not necessarily the one that wrote it.
void putU16(std::uint8_t* at, std::uint16_t value) {
    at[0] = static_cast<std::uint8_t>(value & 0xFF);
    at[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void putU32(std::uint8_t* at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) at[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
}

void putU64(std::uint8_t* at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) at[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
}

std::uint16_t readU16(const std::uint8_t* at) {
    return static_cast<std::uint16_t>(at[0]) | static_cast<std::uint16_t>(at[1] << 8);
}

std::uint32_t readU32(const std::uint8_t* at) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(at[i]) << (8 * i);
    return value;
}

std::uint64_t readU64(const std::uint8_t* at) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(at[i]) << (8 * i);
    return value;
}

/// The chunk index as a 96-bit nonce. Safe as a bare counter only because the
/// key encrypting this file encrypts nothing else — see the header.
std::vector<std::uint8_t> nonceForChunk(std::uint64_t index) {
    std::vector<std::uint8_t> nonce(kNonceBytes, 0);
    putU64(nonce.data(), index);
    return nonce;
}

#if defined(TRACE_WITH_ENCRYPTION)

/// One AES-256-GCM operation. Kept in one place so nonce and tag handling cannot
/// drift between the small-value path and the container path.
Result<std::vector<std::uint8_t>> gcmEncrypt(const SecretKey& key,
                                             const std::vector<std::uint8_t>& nonce,
                                             const std::uint8_t* plaintext, std::size_t bytes,
                                             const std::uint8_t* aad, std::size_t aadBytes,
                                             std::uint8_t* tagOut) {
    using ResultType = Result<std::vector<std::uint8_t>>;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return ResultType::failure(ErrorCode::Internal, "Unable to allocate a cipher context");
    }
    struct Guard {
        EVP_CIPHER_CTX* ctx;
        ~Guard() { EVP_CIPHER_CTX_free(ctx); }
    } guard{ctx};

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
        return ResultType::failure(ErrorCode::Internal, "Unable to start encryption");
    }

    int written = 0;
    if (aadBytes > 0 &&
        EVP_EncryptUpdate(ctx, nullptr, &written, aad, static_cast<int>(aadBytes)) != 1) {
        return ResultType::failure(ErrorCode::Internal, "Unable to authenticate the header");
    }

    std::vector<std::uint8_t> ciphertext(bytes);
    if (bytes > 0 && EVP_EncryptUpdate(ctx, ciphertext.data(), &written, plaintext,
                                       static_cast<int>(bytes)) != 1) {
        return ResultType::failure(ErrorCode::Internal, "Encryption failed");
    }
    int finalBytes = 0;
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + written, &finalBytes) != 1) {
        return ResultType::failure(ErrorCode::Internal, "Encryption failed to finalise");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagBytes), tagOut) != 1) {
        return ResultType::failure(ErrorCode::Internal, "Unable to read the authentication tag");
    }
    return ResultType::success(std::move(ciphertext));
}

Result<std::vector<std::uint8_t>> gcmDecrypt(const SecretKey& key,
                                             const std::vector<std::uint8_t>& nonce,
                                             const std::uint8_t* ciphertext, std::size_t bytes,
                                             const std::uint8_t* aad, std::size_t aadBytes,
                                             const std::uint8_t* tag) {
    using ResultType = Result<std::vector<std::uint8_t>>;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return ResultType::failure(ErrorCode::Internal, "Unable to allocate a cipher context");
    }
    struct Guard {
        EVP_CIPHER_CTX* ctx;
        ~Guard() { EVP_CIPHER_CTX_free(ctx); }
    } guard{ctx};

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
        return ResultType::failure(ErrorCode::Internal, "Unable to start decryption");
    }

    int written = 0;
    if (aadBytes > 0 &&
        EVP_DecryptUpdate(ctx, nullptr, &written, aad, static_cast<int>(aadBytes)) != 1) {
        return ResultType::failure(ErrorCode::IntegrityFailure, "Container header did not verify");
    }

    std::vector<std::uint8_t> plaintext(bytes);
    if (bytes > 0 && EVP_DecryptUpdate(ctx, plaintext.data(), &written, ciphertext,
                                       static_cast<int>(bytes)) != 1) {
        return ResultType::failure(ErrorCode::IntegrityFailure, "Decryption failed");
    }

    // Const-cast because OpenSSL's ctrl interface is not const-correct; the tag
    // is not modified by a GET/SET of the expected value.
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagBytes),
                            const_cast<std::uint8_t*>(tag)) != 1) {
        return ResultType::failure(ErrorCode::Internal, "Unable to set the authentication tag");
    }

    int finalBytes = 0;
    // This is the check that matters: a wrong key, a modified byte and a
    // reordered chunk all land here, and all get the same answer.
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + written, &finalBytes) != 1) {
        return ResultType::failure(
            ErrorCode::IntegrityFailure,
            "Encrypted data did not authenticate",
            "The key is wrong, or the stored bytes have been altered since they were written.");
    }
    return ResultType::success(std::move(plaintext));
}

#endif  // TRACE_WITH_ENCRYPTION

#if !defined(TRACE_WITH_ENCRYPTION)
Status unavailable() {
    return Status::failure(
        ErrorCode::Unsupported, "This build of TRACE was built without encryption support",
        "Rebuild with TRACE_WITH_ENCRYPTION=ON, or open this workspace with a build that has it.");
}
#endif

}  // namespace

const char kContainerMagic[9] = "TRACEEV1";

bool available() {
#if defined(TRACE_WITH_ENCRYPTION)
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------- SecretKey

SecretKey::SecretKey() : bytes_(kKeyBytes, 0) {}

SecretKey::~SecretKey() {
    if (!bytes_.empty()) {
#if defined(TRACE_WITH_ENCRYPTION)
        OPENSSL_cleanse(bytes_.data(), bytes_.size());
#else
        std::fill(bytes_.begin(), bytes_.end(), std::uint8_t{0});
#endif
    }
}

SecretKey::SecretKey(const SecretKey& other) = default;
SecretKey& SecretKey::operator=(const SecretKey& other) = default;
SecretKey::SecretKey(SecretKey&& other) noexcept = default;
SecretKey& SecretKey::operator=(SecretKey&& other) noexcept = default;

Result<SecretKey> SecretKey::random() {
    using ResultType = Result<SecretKey>;
    auto bytes = password::randomBytes(kKeyBytes);
    if (!bytes) return ResultType(bytes.error());
    SecretKey key;
    key.bytes_ = bytes.take();
    return ResultType::success(std::move(key));
}

Result<SecretKey> SecretKey::fromBytes(const std::vector<std::uint8_t>& bytes) {
    using ResultType = Result<SecretKey>;
    if (bytes.size() != kKeyBytes) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A key must be exactly 32 bytes",
                                   "Received " + std::to_string(bytes.size()) + ".");
    }
    SecretKey key;
    key.bytes_ = bytes;
    return ResultType::success(std::move(key));
}

Result<SecretKey> SecretKey::fromPassword(const std::string& plaintext,
                                          const std::vector<std::uint8_t>& salt,
                                          std::uint32_t iterations) {
    using ResultType = Result<SecretKey>;
    if (salt.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A key derivation needs a salt");
    }
    if (iterations == 0) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A key derivation needs a non-zero work factor");
    }
    SecretKey key;
    key.bytes_ = password::pbkdf2Sha256(plaintext, salt, iterations, kKeyBytes);
    return ResultType::success(std::move(key));
}

Result<SecretKey> SecretKey::deriveSubkey(const std::vector<std::uint8_t>& salt,
                                          const std::string& info) const {
    using ResultType = Result<SecretKey>;
    // HKDF-SHA256, RFC 5869. Extract, then one expansion round — which is all
    // that is needed, because the output is exactly one hash length.
    const std::vector<std::uint8_t> ikm(bytes_.begin(), bytes_.end());
    const std::vector<std::uint8_t> prk = password::hmacSha256(salt, ikm);

    std::vector<std::uint8_t> block(info.begin(), info.end());
    block.push_back(0x01);
    SecretKey key;
    key.bytes_ = password::hmacSha256(prk, block);
    if (key.bytes_.size() != kKeyBytes) {
        return ResultType::failure(ErrorCode::Internal,
                                   "Subkey derivation produced the wrong length");
    }
    return ResultType::success(std::move(key));
}

std::string SecretKey::toHexForSqlCipher() const {
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes_.size() * 2);
    for (std::uint8_t byte : bytes_) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0F]);
    }
    return hex;
}

// ------------------------------------------------------------ small values

Result<std::vector<std::uint8_t>> seal(const SecretKey& key,
                                       const std::vector<std::uint8_t>& plaintext,
                                       const std::string& associatedData) {
    using ResultType = Result<std::vector<std::uint8_t>>;
#if !defined(TRACE_WITH_ENCRYPTION)
    (void)key; (void)plaintext; (void)associatedData;
    return ResultType(unavailable().error());
#else
    auto nonce = password::randomBytes(kNonceBytes);
    if (!nonce) return ResultType(nonce.error());
    const std::vector<std::uint8_t> nonceBytes = nonce.take();

    std::vector<std::uint8_t> tag(kTagBytes, 0);
    auto ciphertext = gcmEncrypt(key, nonceBytes, plaintext.data(), plaintext.size(),
                                 reinterpret_cast<const std::uint8_t*>(associatedData.data()),
                                 associatedData.size(), tag.data());
    if (!ciphertext) return ResultType(ciphertext.error());

    std::vector<std::uint8_t> sealed;
    sealed.reserve(nonceBytes.size() + plaintext.size() + kTagBytes);
    sealed.insert(sealed.end(), nonceBytes.begin(), nonceBytes.end());
    const std::vector<std::uint8_t> body = ciphertext.take();
    sealed.insert(sealed.end(), body.begin(), body.end());
    sealed.insert(sealed.end(), tag.begin(), tag.end());
    return ResultType::success(std::move(sealed));
#endif
}

Result<std::vector<std::uint8_t>> unseal(const SecretKey& key,
                                         const std::vector<std::uint8_t>& sealed,
                                         const std::string& associatedData) {
    using ResultType = Result<std::vector<std::uint8_t>>;
#if !defined(TRACE_WITH_ENCRYPTION)
    (void)key; (void)sealed; (void)associatedData;
    return ResultType(unavailable().error());
#else
    if (sealed.size() < kNonceBytes + kTagBytes) {
        return ResultType::failure(ErrorCode::IntegrityFailure,
                                   "Sealed value is too short to be intact");
    }
    const std::vector<std::uint8_t> nonce(sealed.begin(), sealed.begin() + kNonceBytes);
    const std::size_t bodyBytes = sealed.size() - kNonceBytes - kTagBytes;
    return gcmDecrypt(key, nonce, sealed.data() + kNonceBytes, bodyBytes,
                      reinterpret_cast<const std::uint8_t*>(associatedData.data()),
                      associatedData.size(), sealed.data() + kNonceBytes + bodyBytes);
#endif
}

// ------------------------------------------------------------- container IO

namespace {

/// Builds the 48-byte header. Also the associated data for every chunk, so any
/// change to it — a shortened length, a different salt — invalidates the whole
/// file rather than just its first record.
std::vector<std::uint8_t> buildHeader(std::uint32_t chunkBytes, std::uint64_t plainSize,
                                      const std::vector<std::uint8_t>& salt) {
    std::vector<std::uint8_t> header(kContainerHeaderBytes, 0);
    std::memcpy(header.data(), kContainerMagic, 8);
    putU16(header.data() + 8, kVersion);
    putU16(header.data() + 10, kAlgorithmAes256Gcm);
    putU32(header.data() + 12, chunkBytes);
    putU64(header.data() + 16, plainSize);
    std::memcpy(header.data() + 24, salt.data(), salt.size());
    // Bytes 40..47 stay zero: reserved, and authenticated, so a later version
    // cannot repurpose them without every existing file noticing.
    return header;
}

/// Associated data for one chunk: the header, then the index. Binding the index
/// is what stops two chunks of the same file being swapped.
std::vector<std::uint8_t> chunkAad(const std::vector<std::uint8_t>& header, std::uint64_t index) {
    std::vector<std::uint8_t> aad = header;
    aad.resize(header.size() + 8);
    putU64(aad.data() + header.size(), index);
    return aad;
}

}  // namespace

struct EncryptedFileWriter::Impl {
    std::ofstream out;
    SecretKey fileKey;
    std::vector<std::uint8_t> header;
    std::vector<std::uint8_t> buffer;
    std::size_t chunkBytes = kDefaultChunkBytes;
    std::uint64_t chunkIndex = 0;
    std::uint64_t declaredSize = 0;
    std::uint64_t writtenSize = 0;
    bool finished = false;
};

EncryptedFileWriter::EncryptedFileWriter() : impl_(std::make_unique<Impl>()) {}
EncryptedFileWriter::~EncryptedFileWriter() = default;

Status EncryptedFileWriter::begin(const std::filesystem::path& path, const SecretKey& caseKey,
                                  std::uint64_t plaintextSize, std::size_t chunkBytes) {
#if !defined(TRACE_WITH_ENCRYPTION)
    (void)path; (void)caseKey; (void)plaintextSize; (void)chunkBytes;
    return unavailable();
#else
    if (chunkBytes == 0) {
        return Status::failure(ErrorCode::InvalidArgument, "Chunk size must be non-zero");
    }
    auto salt = password::randomBytes(kFileSaltBytes);
    if (!salt) return Status(salt.error());
    const std::vector<std::uint8_t> saltBytes = salt.take();

    auto fileKey = caseKey.deriveSubkey(saltBytes, "trace-evidence-v1");
    if (!fileKey) return Status(fileKey.error());

    impl_->fileKey = fileKey.take();
    impl_->chunkBytes = chunkBytes;
    impl_->declaredSize = plaintextSize;
    impl_->header = buildHeader(static_cast<std::uint32_t>(chunkBytes), plaintextSize, saltBytes);
    impl_->buffer.clear();
    impl_->buffer.reserve(chunkBytes);

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    impl_->out.open(path, std::ios::binary | std::ios::trunc);
    if (!impl_->out) {
        return Status::failure(ErrorCode::IoError,
                               "Unable to create the encrypted file: " + path.string());
    }
    impl_->out.write(reinterpret_cast<const char*>(impl_->header.data()),
                     static_cast<std::streamsize>(impl_->header.size()));
    if (!impl_->out) {
        return Status::failure(ErrorCode::IoError, "Unable to write the container header");
    }
    return Status::success();
#endif
}

Status EncryptedFileWriter::write(const std::uint8_t* data, std::size_t bytes) {
#if !defined(TRACE_WITH_ENCRYPTION)
    (void)data; (void)bytes;
    return unavailable();
#else
    if (!impl_->out.is_open()) {
        return Status::failure(ErrorCode::Internal, "Container is not open for writing");
    }
    std::size_t offset = 0;
    while (offset < bytes) {
        const std::size_t room = impl_->chunkBytes - impl_->buffer.size();
        const std::size_t take = std::min(room, bytes - offset);
        impl_->buffer.insert(impl_->buffer.end(), data + offset, data + offset + take);
        offset += take;
        impl_->writtenSize += take;

        if (impl_->buffer.size() == impl_->chunkBytes) {
            const auto aad = chunkAad(impl_->header, impl_->chunkIndex);
            std::vector<std::uint8_t> tag(kTagBytes, 0);
            auto sealed = gcmEncrypt(impl_->fileKey, nonceForChunk(impl_->chunkIndex),
                                     impl_->buffer.data(), impl_->buffer.size(), aad.data(),
                                     aad.size(), tag.data());
            if (!sealed) return Status(sealed.error());
            const auto body = sealed.take();
            impl_->out.write(reinterpret_cast<const char*>(body.data()),
                             static_cast<std::streamsize>(body.size()));
            impl_->out.write(reinterpret_cast<const char*>(tag.data()),
                             static_cast<std::streamsize>(tag.size()));
            if (!impl_->out) {
                return Status::failure(ErrorCode::IoError, "Unable to write an encrypted chunk");
            }
            impl_->buffer.clear();
            ++impl_->chunkIndex;
        }
    }
    return Status::success();
#endif
}

Status EncryptedFileWriter::finish() {
#if !defined(TRACE_WITH_ENCRYPTION)
    return unavailable();
#else
    if (!impl_->out.is_open()) {
        return Status::failure(ErrorCode::Internal, "Container is not open for writing");
    }
    if (!impl_->buffer.empty()) {
        const auto aad = chunkAad(impl_->header, impl_->chunkIndex);
        std::vector<std::uint8_t> tag(kTagBytes, 0);
        auto sealed = gcmEncrypt(impl_->fileKey, nonceForChunk(impl_->chunkIndex),
                                 impl_->buffer.data(), impl_->buffer.size(), aad.data(), aad.size(),
                                 tag.data());
        if (!sealed) return Status(sealed.error());
        const auto body = sealed.take();
        impl_->out.write(reinterpret_cast<const char*>(body.data()),
                         static_cast<std::streamsize>(body.size()));
        impl_->out.write(reinterpret_cast<const char*>(tag.data()),
                         static_cast<std::streamsize>(tag.size()));
        impl_->buffer.clear();
        ++impl_->chunkIndex;
    }
    impl_->out.flush();
    const bool ok = static_cast<bool>(impl_->out);
    impl_->out.close();
    if (!ok) return Status::failure(ErrorCode::IoError, "Unable to finish the encrypted file");

    // The header promised a length and it is authenticated, so a mismatch here
    // would produce a file that cannot be fully read. Better to say so now than
    // to discover it when the evidence is needed.
    if (impl_->writtenSize != impl_->declaredSize) {
        return Status::failure(ErrorCode::IoError,
                               "Encrypted file is not the length its header declares",
                               "Declared " + std::to_string(impl_->declaredSize) + " bytes, wrote " +
                                   std::to_string(impl_->writtenSize) + ".");
    }
    impl_->finished = true;
    return Status::success();
#endif
}

struct EncryptedFileReader::Impl {
    std::ifstream in;
    SecretKey fileKey;
    std::vector<std::uint8_t> header;
    std::size_t chunkBytes = kDefaultChunkBytes;
    std::uint64_t plainSize = 0;
    std::uint64_t chunkCount = 0;

    /// One decrypted chunk. Sequential decoding asks for far less than a chunk
    /// at a time, so without this every read would decrypt 256 KiB again.
    std::vector<std::uint8_t> cached;
    std::uint64_t cachedIndex = UINT64_MAX;
};

EncryptedFileReader::EncryptedFileReader() : impl_(std::make_unique<Impl>()) {}
EncryptedFileReader::~EncryptedFileReader() = default;

Status EncryptedFileReader::open(const std::filesystem::path& path, const SecretKey& caseKey) {
#if !defined(TRACE_WITH_ENCRYPTION)
    (void)path; (void)caseKey;
    return unavailable();
#else
    impl_->in.open(path, std::ios::binary);
    if (!impl_->in) {
        return Status::failure(ErrorCode::IoError,
                               "Unable to open the encrypted file: " + path.string());
    }
    std::vector<std::uint8_t> header(kContainerHeaderBytes, 0);
    impl_->in.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
    if (impl_->in.gcount() != static_cast<std::streamsize>(kContainerHeaderBytes)) {
        return Status::failure(ErrorCode::IntegrityFailure,
                               "File is too short to be a TRACE encrypted container");
    }
    if (std::memcmp(header.data(), kContainerMagic, 8) != 0) {
        return Status::failure(ErrorCode::IntegrityFailure,
                               "File is not a TRACE encrypted container");
    }
    const std::uint16_t version = readU16(header.data() + 8);
    if (version != kVersion) {
        return Status::failure(ErrorCode::Unsupported,
                               "Unsupported container version",
                               "This file declares version " + std::to_string(version) +
                                   "; this build understands " + std::to_string(kVersion) + ".");
    }
    const std::uint16_t algorithm = readU16(header.data() + 10);
    if (algorithm != kAlgorithmAes256Gcm) {
        return Status::failure(ErrorCode::Unsupported, "Unsupported container algorithm");
    }
    const std::uint32_t chunkBytes = readU32(header.data() + 12);
    if (chunkBytes == 0) {
        return Status::failure(ErrorCode::IntegrityFailure, "Container declares a zero chunk size");
    }

    const std::vector<std::uint8_t> salt(header.begin() + 24, header.begin() + 24 + kFileSaltBytes);
    auto fileKey = caseKey.deriveSubkey(salt, "trace-evidence-v1");
    if (!fileKey) return Status(fileKey.error());

    impl_->fileKey = fileKey.take();
    impl_->header = std::move(header);
    impl_->chunkBytes = chunkBytes;
    impl_->plainSize = readU64(impl_->header.data() + 16);
    impl_->chunkCount = (impl_->plainSize + chunkBytes - 1) / chunkBytes;
    impl_->cachedIndex = UINT64_MAX;
    return Status::success();
#endif
}

std::uint64_t EncryptedFileReader::size() const { return impl_->plainSize; }

Result<std::size_t> EncryptedFileReader::read(std::uint64_t offset, std::uint8_t* into,
                                              std::size_t bytes) {
    using ResultType = Result<std::size_t>;
#if !defined(TRACE_WITH_ENCRYPTION)
    (void)offset; (void)into; (void)bytes;
    return ResultType(unavailable().error());
#else
    if (!impl_->in.is_open()) {
        return ResultType::failure(ErrorCode::Internal, "Container is not open");
    }
    if (offset >= impl_->plainSize || bytes == 0) return ResultType::success(0);

    const std::uint64_t remaining = impl_->plainSize - offset;
    const std::size_t wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(bytes, remaining));

    std::size_t produced = 0;
    while (produced < wanted) {
        const std::uint64_t position = offset + produced;
        const std::uint64_t index = position / impl_->chunkBytes;
        const std::size_t within = static_cast<std::size_t>(position % impl_->chunkBytes);

        if (index != impl_->cachedIndex) {
            // Chunk records are fixed width apart: the last one is shorter in
            // plaintext, but it is also the last, so nothing is indexed past it.
            const std::uint64_t recordBytes =
                static_cast<std::uint64_t>(impl_->chunkBytes) + kTagBytes;
            const std::uint64_t at = kContainerHeaderBytes + index * recordBytes;

            const bool isLast = (index + 1 == impl_->chunkCount);
            const std::size_t plainBytes =
                isLast ? static_cast<std::size_t>(impl_->plainSize -
                                                  index * impl_->chunkBytes)
                       : impl_->chunkBytes;

            std::vector<std::uint8_t> record(plainBytes + kTagBytes);
            impl_->in.clear();
            impl_->in.seekg(static_cast<std::streamoff>(at), std::ios::beg);
            impl_->in.read(reinterpret_cast<char*>(record.data()),
                           static_cast<std::streamsize>(record.size()));
            if (impl_->in.gcount() != static_cast<std::streamsize>(record.size())) {
                return ResultType::failure(
                    ErrorCode::IntegrityFailure, "Encrypted file ends before its header says it does",
                    "Chunk " + std::to_string(index) + " is missing or incomplete.");
            }

            const auto aad = chunkAad(impl_->header, index);
            auto plain = gcmDecrypt(impl_->fileKey, nonceForChunk(index), record.data(), plainBytes,
                                    aad.data(), aad.size(), record.data() + plainBytes);
            if (!plain) return ResultType(plain.error());
            impl_->cached = plain.take();
            impl_->cachedIndex = index;
        }

        const std::size_t fromChunk =
            std::min(impl_->cached.size() - within, wanted - produced);
        std::memcpy(into + produced, impl_->cached.data() + within, fromChunk);
        produced += fromChunk;
    }
    return ResultType::success(produced);
#endif
}

bool looksEncrypted(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[8] = {};
    in.read(magic, sizeof(magic));
    return in.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           std::memcmp(magic, kContainerMagic, sizeof(magic)) == 0;
}

}  // namespace trace::crypto
