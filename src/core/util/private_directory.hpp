#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace zetta::util {

// Owner-only persistence for the cluster and connector roots (#1413).
// Directory-relative operations keep using the verified directory, and writes
// replace the old inode so an earlier reader cannot retain the new secret.
// See docs/cluster/security.md. Internal to zetta-core.
class PrivateDirectory {
public:
    class PendingFile {
    public:
        ~PendingFile();
        PendingFile(const PendingFile&) = delete;
        PendingFile& operator=(const PendingFile&) = delete;
        int descriptor() const { return descriptor_; }

    private:
        friend class PrivateDirectory;
        PendingFile(const PrivateDirectory& directory, std::string name, int descriptor);
        const PrivateDirectory& directory_;
        std::string name_;
        int descriptor_;
    };

    static std::unique_ptr<PrivateDirectory> open(const std::filesystem::path& path,
                                                  std::string* reason);
    ~PrivateDirectory();
    PrivateDirectory(const PrivateDirectory&) = delete;
    PrivateDirectory& operator=(const PrivateDirectory&) = delete;

    // Missing is success with an empty optional; every other read error fails.
    bool read(std::string_view name, std::optional<std::string>& contents,
              std::string* reason) const;
    bool write(std::string_view name, std::string_view contents,
               std::string* reason) const;
    // An empty, exclusive 0600 staging file. Its directory must outlive it.
    std::unique_ptr<PendingFile> create_file(std::string* reason) const;

private:
    PrivateDirectory(std::filesystem::path path, int descriptor);
    std::filesystem::path path_;
    int descriptor_;
};

}  // namespace zetta::util
