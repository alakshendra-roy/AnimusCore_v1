// Hand-rolled native test for the offline RSA license enforcement surface:
// animus_verify_license / animus_check_license_status / animus_is_licensed /
// animus_licensed_max_cores (AnimusCore_v1/animus_engine.cpp), and the
// opt-in SecureExecutionGateway license gate
// (SecureExecutionGateway::set_execution_license_required, animus_security.hpp).
//
// This repo has no C++ test framework (CLAUDE.md's zero-dependency rule
// applies to the native side too, same reasoning as animus/bindings.py's
// own zero-third-party-dependency Python SDK) -- asserts + a PASS/FAIL
// summary + a non-zero exit code on any failure, same style as
// AnimusCore_v1/release_header_smoke_test.cpp. Links against the compiled
// native DLL's import library (a real consumer, exactly like any other
// C++ program embedding this SDK -- see QUICKSTART.md guide 2), since this
// test exercises the DLL-only license/RBAC surface, not just the
// header-only animus::Engine.
//
// Build (after building AnimusCore_v1.dll or AnimusNative.dll -- CMake or
// the vcxproj), from the tests/ directory:
//   cl /std:c++17 /EHsc /O2 /I ..\AnimusCore_v1 test_licensing.cpp ^
//      /Fe:test_licensing.exe ^
//      /link ..\AnimusCore_v1\x64\Release\AnimusCore_v1.lib
//   .\test_licensing.exe
//
// (Swap in ..\build\Release\AnimusNative.lib if that's the build you have.)
// Windows-only, like animus_verify_license itself.
#include "../AnimusCore_v1/animus.hpp"
#include "../AnimusCore_v1/animus_security.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

using namespace animus;
using namespace animus::security;

static int g_failures = 0;

static void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

int main() {
#if !defined(_WIN32)
    std::puts("test_licensing: skipped (license verification is Windows-only, see animus_verify_license's own docstring)");
    return 0;
#else
    std::printf("test_licensing: missing file\n");
    check(animus_check_license_status("this_license_file_does_not_exist.lic") == LicenseStatus::Missing,
        "missing license file -> LicenseStatus::Missing");
    check(!animus_verify_license("this_license_file_does_not_exist.lic"),
        "missing license file -> animus_verify_license() false");
    check(!animus_is_licensed(), "animus_is_licensed() still false so far");

    std::printf("test_licensing: malformed (wrong size) file\n");
    {
        const char* path = "test_licensing_truncated.lic";
        std::ofstream f(path, std::ios::binary);
        f.write("short", 5);
        f.close();
        check(animus_check_license_status(path) == LicenseStatus::Malformed,
            "truncated file -> LicenseStatus::Malformed");
        std::remove(path);
    }

    // Committed, safe fixture: validly signed by the real private key, but
    // for a fingerprint that can never match a real machine -- exercises
    // the fingerprint-mismatch path specifically, distinct from a corrupt
    // signature (tested just below by tampering with a copy of it).
    const char* wrong_machine_path = "../AnimusCore_v1/license_tools/test_fixtures/wrong_machine_test_license.lic";
    std::printf("test_licensing: wrong-machine fixture (committed, safe)\n");
    std::vector<unsigned char> wrong_machine_bytes;
    {
        std::ifstream probe(wrong_machine_path, std::ios::binary);
        if (probe) {
            wrong_machine_bytes.assign(std::istreambuf_iterator<char>(probe), std::istreambuf_iterator<char>());
        }
    }
    if (!wrong_machine_bytes.empty()) {
        check(animus_check_license_status(wrong_machine_path) == LicenseStatus::WrongMachine,
            "wrong-machine fixture -> LicenseStatus::WrongMachine");
        check(!animus_verify_license(wrong_machine_path), "wrong-machine fixture -> animus_verify_license() false");

        std::printf("test_licensing: tampered copy of the wrong-machine fixture\n");
        std::vector<unsigned char> tampered = wrong_machine_bytes;
        tampered.back() ^= 0xFF; // flip a byte inside the RSA signature
        const char* tampered_path = "test_licensing_tampered.lic";
        std::ofstream out(tampered_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(tampered.data()), static_cast<std::streamsize>(tampered.size()));
        out.close();
        check(animus_check_license_status(tampered_path) == LicenseStatus::BadSignature,
            "tampered signature -> LicenseStatus::BadSignature");
        std::remove(tampered_path);
    } else {
        std::printf("  [SKIP] wrong-machine fixture not found at %s\n", wrong_machine_path);
    }

    // A real, valid-for-this-machine license is NOT committed (gitignored,
    // see license_tools/private/) -- skip, not fail, when it hasn't been
    // generated locally, same convention tests/test_bindings.py's own
    // _LOCAL_TEST_LICENSE already uses.
    const char* local_license_path = "../AnimusCore_v1/license_tools/private/test_license_for_this_machine.lic";
    std::printf("test_licensing: local valid license for this machine (optional)\n");
    bool have_local_license = false;
    {
        std::ifstream probe(local_license_path, std::ios::binary);
        if (probe) {
            probe.close();
            check(animus_check_license_status(local_license_path) == LicenseStatus::Valid,
                "local test license -> LicenseStatus::Valid");
            check(animus_is_licensed(), "animus_is_licensed() true after a valid license");
            check(animus_licensed_max_cores() > 0, "animus_licensed_max_cores() > 0 after a valid license");
            have_local_license = animus_is_licensed();
        } else {
            std::printf("  [SKIP] no local test license for this machine -- run "
                "\"python scripts/generate_license.py sign --out ...\" or license_tools/sign_license.ps1 "
                "(same convention as tests/test_bindings.py's own _LOCAL_TEST_LICENSE)\n");
        }
    }

    // SecureExecutionGateway's opt-in license requirement -- header-only,
    // so this is constructed directly rather than through the
    // SecurityContext C-ABI (animus_security_create_context etc.), but it
    // still calls the real DLL-implemented animus_is_licensed() to decide,
    // so this exercises the actual gate, not a reimplementation of it.
    std::printf("test_licensing: SecureExecutionGateway's opt-in execution license requirement\n");
    {
        TenantRegistry registry;
        registry.create_tenant(900);
        SecureExecutionGateway gateway(registry);

        AccessToken admin{ 900, 1, Role::Admin };
        AccessToken operator_token{ 900, 2, Role::Operator };
        check(gateway.create_execution_tenant(admin, 900), "execution tenant 900 created");

        OrderRequest order{};
        order.client_order_id = 1;
        order.instrument_id = 7;
        order.side = OrderSide::Buy;
        order.type = OrderType::Market;
        order.price_ticks = 100;
        order.quantity = 1;
        ExecutionReport report{};

        // Default: OFF -- submit() behaves exactly as it did before this
        // gate existed, regardless of animus_is_licensed(), preserving
        // the already-shipped (v1.1.0-rc1), already-tested behavior.
        check(gateway.submit(operator_token, order, report),
            "submit succeeds with the license requirement OFF (default, unchanged)");

        gateway.set_execution_license_required(true);
        if (!animus_is_licensed()) {
            check(!gateway.submit(operator_token, order, report),
                "submit denied with the requirement ON and no verified license");
        } else {
            std::printf("  [SKIP] a license was already verified earlier in this process (via the local test "
                "license above) -- cannot exercise the 'no license' denial path in the same process\n");
        }
        if (have_local_license) {
            check(gateway.submit(operator_token, order, report),
                "submit succeeds with the requirement ON once a valid license is verified");
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
        g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
#endif
}
