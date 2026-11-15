// Sealed connector credentials (#1230, connect/credential_seal.hpp): AES-256-GCM
// under a key derived from a root secret held outside the replicated catalogue.
#include <gtest/gtest.h>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/connect/credential_seal.hpp"

using zetta::connect::CredentialKeyring;
using zetta::connect::is_sealed_credential;
using zetta::connect::seal_credential;
using zetta::connect::sealed_key_id;
using zetta::connect::unseal_credential;

namespace {

CredentialKeyring keyring_of(const std::string& secret,
                             const std::vector<std::string>& retired = {}) {
    auto keyring = CredentialKeyring::from_root_secrets(secret, retired);
    EXPECT_TRUE(keyring.has_value());
    return keyring ? *keyring : CredentialKeyring{};
}

}  // namespace

TEST(CredentialSeal, SealsAndOpensWithoutCarryingThePlaintext) {
    const CredentialKeyring keyring = keyring_of("root-secret-a");
    const auto sealed = seal_credential(keyring, "hunter2-password", "warehouse");
    ASSERT_TRUE(sealed.has_value());
    EXPECT_TRUE(is_sealed_credential(*sealed));
    EXPECT_EQ(sealed->dump().find("hunter2"), std::string::npos) << sealed->dump();
    EXPECT_EQ(sealed_key_id(*sealed), keyring.current()->id);

    const auto opened = unseal_credential(keyring, *sealed, "warehouse");
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(*opened, "hunter2-password");
}

TEST(CredentialSeal, EverySealUsesAFreshNonce) {
    const CredentialKeyring keyring = keyring_of("root-secret-a");
    const auto first = seal_credential(keyring, "same", "c");
    const auto second = seal_credential(keyring, "same", "c");
    ASSERT_TRUE(first && second);
    EXPECT_NE(first->at("nonce"), second->at("nonce"));
    EXPECT_NE(first->at("ciphertext"), second->at("ciphertext"));
}

TEST(CredentialSeal, TheSameRootSecretDerivesTheSameKeyOnEveryNode) {
    // Two members holding the same join token open each other's seals.
    const auto sealed = seal_credential(keyring_of("join-token"), "token", "c");
    ASSERT_TRUE(sealed);
    EXPECT_EQ(unseal_credential(keyring_of("join-token"), *sealed, "c"), "token");
}

TEST(CredentialSeal, AnotherKeyAnotherConnectionOrATamperedValueDoesNotOpen) {
    const CredentialKeyring keyring = keyring_of("root-secret-a");
    const auto sealed = seal_credential(keyring, "secret-value", "warehouse");
    ASSERT_TRUE(sealed);

    std::string reason;
    EXPECT_FALSE(unseal_credential(keyring_of("root-secret-b"), *sealed, "warehouse",
                                   &reason));
    EXPECT_NE(reason.find("does not hold"), std::string::npos) << reason;
    // Bound to the connection name: copied onto another row, it does not open.
    EXPECT_FALSE(unseal_credential(keyring, *sealed, "other", &reason));
    nlohmann::json tampered = *sealed;
    std::string ciphertext = tampered["ciphertext"].get<std::string>();
    ciphertext[0] = ciphertext[0] == 'a' ? 'b' : 'a';
    tampered["ciphertext"] = ciphertext;
    EXPECT_FALSE(unseal_credential(keyring, tampered, "warehouse", &reason));
    // Not the sealed shape at all.
    EXPECT_FALSE(unseal_credential(keyring, nlohmann::json{{"v", 1}}, "warehouse"));
    EXPECT_FALSE(is_sealed_credential(nlohmann::json("secret-value")));
}

TEST(CredentialSeal, ARetiredKeyStillOpensButNeverSeals) {
    const auto old_seal = seal_credential(keyring_of("old"), "value", "c");
    ASSERT_TRUE(old_seal);
    const CredentialKeyring rotated = keyring_of("new", {"old"});
    EXPECT_EQ(unseal_credential(rotated, *old_seal, "c"), "value");
    const auto new_seal = seal_credential(rotated, "value", "c");
    ASSERT_TRUE(new_seal);
    EXPECT_EQ(sealed_key_id(*new_seal), rotated.current()->id);
    EXPECT_NE(sealed_key_id(*new_seal), sealed_key_id(*old_seal));
}

TEST(CredentialSeal, AnEmptyKeyringSealsNothing) {
    std::string reason;
    EXPECT_FALSE(CredentialKeyring::from_root_secrets("", {}, &reason));
    EXPECT_FALSE(seal_credential(CredentialKeyring{}, "value", "c", &reason));
    EXPECT_FALSE(reason.empty());
}

TEST(CredentialSeal, TheLocalRootSecretIsCreatedPrivateAndReused) {
    zetta::test::TempDir dir;
    const auto first = zetta::connect::load_or_create_local_keyring(dir.path().string());
    ASSERT_TRUE(first);
    const std::filesystem::path file =
        dir.path() / "security" / "connection-root-secret";
    ASSERT_TRUE(std::filesystem::exists(file));
    const auto permissions = std::filesystem::status(file).permissions();
    EXPECT_EQ(permissions & (std::filesystem::perms::group_all |
                             std::filesystem::perms::others_all),
              std::filesystem::perms::none);
    const auto second = zetta::connect::load_or_create_local_keyring(dir.path().string());
    ASSERT_TRUE(second);
    EXPECT_EQ(first->current()->id, second->current()->id);

    // A damaged file is a failure, never silently replaced (that would orphan
    // every credential sealed under it).
    std::ofstream(file, std::ios::trunc) << "not-hex";
    std::string reason;
    EXPECT_FALSE(zetta::connect::load_or_create_local_keyring(dir.path().string(),
                                                              &reason));
    EXPECT_FALSE(reason.empty());
}

TEST(CredentialSeal, LocalRootDirectoryIsPrivateAndLegacyPermissionsAreRestricted) {
    zetta::test::TempDir temporary;
    const mode_t previous_umask = ::umask(0);
    const auto first = zetta::connect::load_or_create_local_keyring(temporary.path().string());
    ::umask(previous_umask);
    ASSERT_TRUE(first);
    const auto directory = temporary.path() / "security";
    const auto path = directory / "connection-root-secret";
    struct stat status {};
    ASSERT_EQ(::stat(directory.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0700);
    ASSERT_EQ(::chmod(directory.c_str(), 0777), 0);
    ASSERT_EQ(::chmod(path.c_str(), 0666), 0);
    ASSERT_TRUE(zetta::connect::load_or_create_local_keyring(temporary.path().string()));
    ASSERT_EQ(::stat(directory.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0700);
    ASSERT_EQ(::stat(path.c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
}

TEST(CredentialSeal, LocalRootRefusesLinksAndSpecialFiles) {
    zetta::test::TempDir temporary;
    const auto directory = temporary.path() / "security";
    std::filesystem::create_directory(directory);
    const auto path = directory / "connection-root-secret";
    const auto outside = temporary.path() / "outside";
    std::ofstream(outside) << std::string(64, 'a');
    std::filesystem::create_symlink(outside, path);
    EXPECT_FALSE(zetta::connect::load_or_create_local_keyring(temporary.path().string()));
    std::filesystem::remove(path);
    std::filesystem::create_hard_link(outside, path);
    EXPECT_FALSE(zetta::connect::load_or_create_local_keyring(temporary.path().string()));
    std::filesystem::remove(path);
    ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
    EXPECT_FALSE(zetta::connect::load_or_create_local_keyring(temporary.path().string()));
}

// #1314: the root secret a node started by main seals under comes from a file
// outside data_dir. One inside it (directly, or through a symbolic link, as a
// Secret mount is links) is refused, as are a writable-by-others file and a
// short secret; later lines are retired secrets.
TEST(CredentialSeal, TheExternalRootSecretMustLiveOutsideTheDataDirectory) {
    zetta::test::TempDir temporary;
    const auto data_dir = temporary.path() / "data";
    const auto mount = temporary.path() / "mounted-secret";
    std::filesystem::create_directories(data_dir / "security");
    std::filesystem::create_directories(mount);
    const std::string current(64, 'c');
    const std::string retired(64, 'r');
    const auto secret_file = mount / "root";
    std::ofstream(secret_file) << "# connector root secret\n" << current << "\n\n" << retired << "\n";
    std::filesystem::permissions(secret_file, std::filesystem::perms::owner_read);

    std::string reason;
    const auto loaded = zetta::connect::load_external_root_secret(
        secret_file.string(), data_dir.string(), &reason);
    ASSERT_TRUE(loaded) << reason;
    EXPECT_EQ(loaded->current, current);
    ASSERT_EQ(loaded->retired.size(), 1u);
    EXPECT_EQ(loaded->retired.front(), retired);

    // Inside data_dir, directly or through a link that resolves there.
    const auto inside = data_dir / "security" / "root";
    std::ofstream(inside) << current << "\n";
    std::filesystem::permissions(inside, std::filesystem::perms::owner_read);
    EXPECT_FALSE(zetta::connect::load_external_root_secret(inside.string(),
                                                           data_dir.string(), &reason));
    EXPECT_NE(reason.find("inside the data directory"), std::string::npos) << reason;
    const auto link = mount / "link";
    std::filesystem::create_symlink(inside, link);
    EXPECT_FALSE(zetta::connect::load_external_root_secret(link.string(),
                                                           data_dir.string(), &reason));
    EXPECT_FALSE(zetta::connect::load_external_root_secret(
        inside.string(), data_dir.string() + "/", &reason));

    // Writable by others, too short, empty, missing.
    const auto loose = mount / "loose";
    std::ofstream(loose) << current << "\n";
    std::filesystem::permissions(loose, std::filesystem::perms::owner_read |
                                            std::filesystem::perms::owner_write |
                                            std::filesystem::perms::others_write);
    EXPECT_FALSE(zetta::connect::load_external_root_secret(loose.string(), data_dir.string()));
    const auto short_file = mount / "short";
    std::ofstream(short_file) << "too-short\n";
    EXPECT_FALSE(
        zetta::connect::load_external_root_secret(short_file.string(), data_dir.string()));
    const auto empty_file = mount / "empty";
    std::ofstream(empty_file) << "# nothing\n";
    EXPECT_FALSE(
        zetta::connect::load_external_root_secret(empty_file.string(), data_dir.string()));
    EXPECT_FALSE(zetta::connect::load_external_root_secret((mount / "absent").string(),
                                                           data_dir.string()));
}
