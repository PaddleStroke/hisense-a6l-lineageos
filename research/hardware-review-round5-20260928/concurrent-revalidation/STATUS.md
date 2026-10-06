# Concurrent integration revalidation

After the complete successful radio run, the final source-hash comparison detected edits to QMI `client.cc/.h`, `datacall.cc/.h`, `services.cc/.h`, and `RadioMessagingVoice.cpp`.

The subsequent snapshot retained here compiled and reproduced the contract tests (F42/F44/F45/F46/F48). Building the complete ModemCore identity test then failed with this compiler diagnostic:

```text
radio/hal/ModemCore.cpp:136:19: error: cannot convert
ModemCore::start()::<lambda(int)> to DataCallManager::LostFn
{aka std::function<void(int, long unsigned int)>}
    mData->onLost([this](int cid) {
```

The snapshot's `radio/qmi/include/a6lqmi/datacall.h:117` expects the new two-argument callback. This is an in-progress source integration mismatch, not a fake-modem runtime failure and not an additional numbered hardware finding. No production file was changed to make the review harness compile.

The next source read showed the callback had already become `(int cid, uint64_t gen)`, so the specific mismatch was transient. Thirteen source files differed by the final comparison. The main evidence files and `initial-radio-evidence/` preserve the earlier complete run in which F43 and all other radio reproductions passed. Do not interpret that as validation of the continuing integration. Rerun against its completed source snapshot. No current-build acceptance is claimed.
