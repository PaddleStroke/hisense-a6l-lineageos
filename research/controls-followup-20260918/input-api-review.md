# Native input integration review

Pinned tree: `/home/a6l/android/a6l-lineage24` (Lineage 24-era AOSP). The smallest real Android pipeline is `EventHub -> InputReader -> InputDispatcher -> InputChannel -> InputConsumer`, with the dispatcher receiving `WindowInfosUpdate` from SurfaceFlinger. The production reference for the SurfaceFlinger/channel half is `frameworks/native/libs/gui/tests/EndToEndNativeInputTest.cpp` (especially `InputSurface`, lines 108–310).

## Required pieces

1. Construct `inputdispatcher::InputDispatcher` with an `InputDispatcherPolicyInterface`, call `start()`, then `createInputChannel(name)`. Keep the returned client `InputChannel`; put `channel->getConnectionToken()` in `gui::WindowInfo::token`.
2. Make `gui::WindowInfoHandle`; set name, dispatch timeout, global scale, full-display touchable region and `InputApplicationInfo`. Attach it atomically with `SurfaceComposerClient::Transaction::setInputWindowInfo(surface, info)` (`libs/gui/SurfaceComposerClient.cpp:2015`). Its public declaration is in `include/gui/SurfaceComposerClient.h:728`.
3. Primary correction: the pinned `InputDispatcher` constructor already registers its own `DispatcherWindowListener`, forwards the initial snapshot, and unregisters on destruction (`dispatcher/InputDispatcher.cpp:935–958`). Do not add a duplicate forwarding listener. Explicitly call `setInputDispatchMode(true, false)` because dispatch starts disabled.
4. Make an active internal `DisplayViewport` for logical display 0: logical/physical bounds and device size 1080×2340, rotation 0, `uniqueId="local:0"`. Return it from `InputReaderPolicyInterface::getReaderConfiguration()` and `getPointerViewportForAssociatedDisplay()`. `DisplayViewport` fields are in `include/input/DisplayViewport.h`.
5. Create `InputReader(std::make_shared<EventHub>(), readerPolicy, dispatcher, nullptr, nullptr)` and start it after the dispatcher. This uses the genuine EventHub input-device scan and IDC calibration. InputManager’s corresponding production construction is `services/inputflinger/InputManager.cpp:160–190`.
6. Consume with `InputConsumer::consume(factory, true, -1, &seq, &event)` and **always** call `sendFinishedSignal(seq, true)` for every successful consumption, including focus/touch-mode events. The pinned end-to-end implementation is `EndToEndNativeInputTest.cpp:243–267`.

## Policy and build constraint

`InputReader` and `InputDispatcher` are private framework components, not an NDK/vendor API. A standalone client must supply the nine reader callbacks from `InputReaderPolicyInterface` (`services/inputflinger/include/InputReaderBase.h:474–523`) and the dispatcher-policy callbacks (`dispatcher/include/InputDispatcherPolicyInterface.h`). For this diagnostic they can be no-op except both `intercept*BeforeQueueing` methods must set `POLICY_FLAG_PASS_TO_USER`, and `filterInputEvent` returns true. This is materially more code than the SF side, but does not require a mock.

The executable must be a platform/private-root module, with private include directories for `services/inputflinger/{include,reader/include,dispatcher}` and dependencies `libinputflinger`, `libinput`, `libgui`, `libbinder`, `libui`, `libutils`, `libcutils`, `liblog`. Primary compiler checks additionally required `libkll`, `libstatslog_inputflinger`, `libstatspull`, `libstatssocket`, `libinputreporter_headers`, and `libbatteryservice_headers`. It cannot be a `vendor: true` module.

The new `device/hisense/a6l/diagnostic/input_client.cpp` and `input_policy.h` implement this route. It waits for its caller’s `/logs/finish`, has a 180-second alarm, creates `/logs/input-ready` only after SF metadata, reader and dispatcher are live, writes motion/pointer count evidence, marks all four tap quadrants green, and ACKs each dispatch.

The client submits its own `setInputWindowInfo` transaction, but feeds its own dispatcher from the actual SurfaceFlinger listener rather than fabricating a window list. The existing system InputFlinger may also observe the same physical node; the private-root fixture must ensure it does not start a competing input service if duplicate delivery is undesirable.
