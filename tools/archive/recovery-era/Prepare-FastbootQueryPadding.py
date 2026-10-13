#!/usr/bin/env python3
"""Prepare a guarded host-only fixed-length query experiment in pinned AOSP source."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('core', type=Path)
args = parser.parse_args()
assert subprocess.check_output(['git', '-C', str(args.core), 'rev-parse', 'HEAD'], text=True).strip() == '1375cc767964bbac2070bc41ebc3c864589846b4'
driver = args.core / 'fastboot/fastboot_driver.cpp'
tests = args.core / 'fastboot/fastboot_driver_test.cpp'
original_driver = subprocess.check_output(['git', '-C', str(args.core), 'show', 'HEAD:fastboot/fastboot_driver.cpp'], text=True)
original_tests = subprocess.check_output(['git', '-C', str(args.core), 'show', 'HEAD:fastboot/fastboot_driver_test.cpp'], text=True)
old = '''    if (transport_->Write(cmd.c_str(), cmd.size()) != static_cast<int>(cmd.size())) {
        error_ = ErrnoStr("Write to device failed");'''
new = '''    // A6L host-only experiment: opt-in padding, restricted to fixed queries and reboot.
    // Never use this mode to download data, unlock, erase, or write partitions.
    std::string wire_cmd = cmd;
    const char* a6l_padding = getenv("A6L_QUERY_PAD64");
    if (a6l_padding && strcmp(a6l_padding, "1") == 0) {
        const bool allowed = cmd == "getvar:product" || cmd == "getvar:unlocked" ||
                cmd == "getvar:secure" || cmd == "getvar:max-download-size" ||
                cmd == "getvar:partition-size:recovery" || cmd == "getvar:partition-size:boot" ||
                cmd == "getvar:partition-size:system" || cmd == "getvar:all" || cmd == "reboot";
        if (!allowed) {
            error_ = "A6L query experiment rejects commands outside its fixed allowlist";
            return BAD_ARG;
        }
        wire_cmd.resize(64, '\\0');
    }
    if (transport_->Write(wire_cmd.data(), wire_cmd.size()) != static_cast<int>(wire_cmd.size())) {
        error_ = ErrnoStr("Write to device failed");'''
assert original_driver.count(old) == 1
prepared_driver = original_driver.replace(old, new)
extra_tests = r'''

namespace {
class A6LQueryPaddingEnvironment {
  public:
    A6LQueryPaddingEnvironment() {
        const char* previous = getenv("A6L_QUERY_PAD64");
        had_previous_ = previous != nullptr;
        if (previous) previous_ = previous;
        setenv("A6L_QUERY_PAD64", "1", 1);
    }
    ~A6LQueryPaddingEnvironment() {
        if (had_previous_) setenv("A6L_QUERY_PAD64", previous_.c_str(), 1);
        else unsetenv("A6L_QUERY_PAD64");
    }
  private:
    bool had_previous_;
    std::string previous_;
};
}  // namespace

TEST_F(DriverTest, A6LQueryPaddingWireBytes) {
    A6LQueryPaddingEnvironment environment;
    auto pointer = std::make_unique<MockTransport>();
    MockTransport* transport = pointer.get();
    FastBootDriver driver(std::move(pointer));
    std::string expected = "getvar:product";
    expected.resize(64, '\0');
    EXPECT_CALL(*transport, Write(_, _))
            .With(AllArgs(RawData(std::string_view(expected))))
            .WillOnce(ReturnArg<1>());
    EXPECT_CALL(*transport, Read(_, _)).WillOnce(Invoke(CopyData("OKAYsdm660")));
    std::string output;
    ASSERT_EQ(driver.GetVar("product", &output), SUCCESS);
    EXPECT_EQ(output, "sdm660");
}

TEST_F(DriverTest, A6LQueryPaddingRejectsWrites) {
    A6LQueryPaddingEnvironment environment;
    auto pointer = std::make_unique<MockTransport>();
    MockTransport* transport = pointer.get();
    FastBootDriver driver(std::move(pointer));
    EXPECT_CALL(*transport, Write(_, _)).Times(0);
    EXPECT_CALL(*transport, Read(_, _)).Times(0);
    for (const char* command : {"download:00001000", "flash:recovery", "erase:userdata",
                                "Hisense unlock", "flashing unlock", "continue"}) {
        std::string response;
        EXPECT_EQ(driver.RawCommand(command, &response), BAD_ARG);
    }
}

TEST_F(DriverTest, A6LQueryPaddingReboot) {
    A6LQueryPaddingEnvironment environment;
    auto pointer = std::make_unique<MockTransport>();
    MockTransport* transport = pointer.get();
    FastBootDriver driver(std::move(pointer));
    std::string expected = "reboot";
    expected.resize(64, '\0');
    EXPECT_CALL(*transport, Write(_, _))
            .With(AllArgs(RawData(std::string_view(expected))))
            .WillOnce(ReturnArg<1>());
    EXPECT_CALL(*transport, Read(_, _)).WillOnce(Invoke(CopyData("OKAY")));
    EXPECT_EQ(driver.Reboot(), SUCCESS);
}
'''
legacy_prepared_tests = original_tests + extra_tests
prepared_tests = original_tests + '\n#if !defined(_WIN32)\n' + extra_tests + '\n#endif\n'
for file, original, prepared in [(driver, original_driver, prepared_driver), (tests, original_tests, prepared_tests)]:
    allowed = (original, prepared, legacy_prepared_tests) if file == tests else (original, prepared)
    assert file.read_text() in allowed, f'Refusing to overwrite unrelated changes: {file}'
driver.write_text(prepared_driver)
tests.write_text(prepared_tests)
print('Prepared opt-in query padding and mock-transport tests; no phone access')
