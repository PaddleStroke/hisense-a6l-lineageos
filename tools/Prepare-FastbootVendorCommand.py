#!/usr/bin/env python3
"""Add a guarded Hisense command to the pinned local AOSP host client; no phone access."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('core', type=Path)
args = parser.parse_args()
assert subprocess.check_output(['git', '-C', str(args.core), 'rev-parse', 'HEAD'], text=True).strip() == '1375cc767964bbac2070bc41ebc3c864589846b4'
main = args.core / 'fastboot/fastboot.cpp'
driver = args.core / 'fastboot/fastboot_driver.cpp'
tests = args.core / 'fastboot/fastboot_driver_test.cpp'
original = subprocess.check_output(['git', '-C', str(args.core), 'show', 'HEAD:fastboot/fastboot.cpp'], text=True)
anchor = '''        } else if (command == FB_CMD_OEM) {
            do_oem_command(fp->fb, FB_CMD_OEM, &args);'''
replacement = '''        } else if (command == "Hisense") {
            // A6L vendor command, disabled unless explicitly enabled by the operator.
            const char* enabled = getenv("A6L_VENDOR_UNLOCK");
            if (!enabled || strcmp(enabled, "1") != 0 || args.size() != 1 ||
                args[0] != "unlock") {
                die("A6L vendor unlock requires A6L_VENDOR_UNLOCK=1 and exactly Hisense unlock");
            }
            do_oem_command(fp->fb, "Hisense", &args);
        } else if (command == FB_CMD_OEM) {
            do_oem_command(fp->fb, FB_CMD_OEM, &args);'''
assert original.count(anchor) == 1
prepared_main = original.replace(anchor, replacement)
assert main.read_text() in (original, prepared_main), 'Unrelated fastboot.cpp changes'

driver_marker = '    // A6L host-only experiment: opt-in padding, restricted to fixed queries and reboot.'
driver_addition = '''    // A6L_VENDOR_COMMAND_GUARD: protect even direct RawCommand callers.
    if (cmd == "Hisense unlock") {
        const char* enabled = getenv("A6L_VENDOR_UNLOCK");
        if (!enabled || strcmp(enabled, "1") != 0) {
            error_ = "A6L vendor unlock is not enabled";
            return BAD_ARG;
        }
    }
'''
current_driver = driver.read_text()
assert current_driver.count(driver_marker) == 1, 'Expected the preserved query experiment'
if 'A6L_VENDOR_COMMAND_GUARD' in current_driver:
    assert driver_addition + driver_marker in current_driver
    prepared_driver = current_driver
else:
    prepared_driver = current_driver.replace(driver_marker, driver_addition + driver_marker)

extra_tests = r'''

#if !defined(_WIN32)
// A6L_VENDOR_COMMAND_TESTS
namespace {
class A6LVendorEnvironment {
  public:
    explicit A6LVendorEnvironment(bool enabled) {
        const char* old = getenv("A6L_VENDOR_UNLOCK");
        present_ = old != nullptr;
        if (old) old_ = old;
        if (enabled) setenv("A6L_VENDOR_UNLOCK", "1", 1);
        else unsetenv("A6L_VENDOR_UNLOCK");
    }
    ~A6LVendorEnvironment() {
        if (present_) setenv("A6L_VENDOR_UNLOCK", old_.c_str(), 1);
        else unsetenv("A6L_VENDOR_UNLOCK");
    }
  private:
    bool present_;
    std::string old_;
};
}

TEST_F(DriverTest, A6LVendorCommandDisabledByDefault) {
    A6LVendorEnvironment environment(false);
    auto pointer = std::make_unique<MockTransport>();
    MockTransport* transport = pointer.get();
    FastBootDriver driver(std::move(pointer));
    EXPECT_CALL(*transport, Write(_, _)).Times(0);
    EXPECT_CALL(*transport, Read(_, _)).Times(0);
    std::string response;
    EXPECT_EQ(driver.RawCommand("Hisense unlock", &response), BAD_ARG);
}

TEST_F(DriverTest, A6LVendorCommandExactWireBytes) {
    A6LVendorEnvironment environment(true);
    auto pointer = std::make_unique<MockTransport>();
    MockTransport* transport = pointer.get();
    FastBootDriver driver(std::move(pointer));
    EXPECT_CALL(*transport, Write(_, _))
            .With(AllArgs(RawData(std::string_view("Hisense unlock"))))
            .WillOnce(ReturnArg<1>());
    EXPECT_CALL(*transport, Read(_, _)).WillOnce(Invoke(CopyData("OKAY")));
    std::string response;
    EXPECT_EQ(driver.RawCommand("Hisense unlock", &response), SUCCESS);
}
#endif
'''
current_tests = tests.read_text()
assert 'TEST_F(DriverTest, A6LQueryPaddingWireBytes)' in current_tests
if 'A6L_VENDOR_COMMAND_TESTS' in current_tests:
    assert current_tests.endswith(extra_tests)
    prepared_tests = current_tests
else:
    prepared_tests = current_tests + extra_tests

main.write_text(prepared_main)
driver.write_text(prepared_driver)
tests.write_text(prepared_tests)
print('Prepared guarded host-only Hisense command and two transport tests; no phone access')
