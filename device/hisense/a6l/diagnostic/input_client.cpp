// Standalone, bounded proof that EventHub -> InputReader -> InputDispatcher -> InputChannel works.
#include "input_policy.h"

#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include <android/os/BnInputFlinger.h>
#include <dispatcher/InputDispatcher.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <gui/Surface.h>
#include <gui/WindowInfosListener.h>
#include <input/InputConsumer.h>
#include <input/InputTransport.h>
#include <EventHub.h>
#include <InputReader.h>
#include <ui/Rect.h>
#include <unistd.h>

#include <fcntl.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>

using namespace android;
using namespace std::chrono_literals;

namespace {
constexpr int kWidth = 1080;
constexpr int kHeight = 2340;
constexpr int kMarkerSize = 96;

void fail(const char* what) {
    std::fprintf(stderr, "A6L_INPUT_FAIL %s\n", what);
    std::fflush(stderr);
    std::_Exit(1);
}

// SurfaceFlinger uses this real dispatcher service for focus commands and as
// the prerequisite for publishing input-window updates to its listeners.
class InputService final : public os::BnInputFlinger {
public:
    explicit InputService(inputdispatcher::InputDispatcher& dispatcher) : mDispatcher(dispatcher) {}
    binder::Status createInputChannel(const std::string& name, os::InputChannelCore* out) override {
        const uid_t uid=IPCThreadState::self()->getCallingUid();
        if(uid!=0 && uid!=2000)return binder::Status::fromExceptionCode(binder::Status::EX_SECURITY);
        auto channel=mDispatcher.createInputChannel(name);
        if(!channel.ok())return binder::Status::fromServiceSpecificError(channel.error().code());
        InputChannel::moveChannel(std::move(*channel),*out);
        return binder::Status::ok();
    }
    binder::Status removeInputChannel(const sp<IBinder>& token) override {
        mDispatcher.removeInputChannel(token);return binder::Status::ok();
    }
    binder::Status setFocusedWindow(const gui::FocusRequest& request) override {
        mDispatcher.setFocusedWindow(request);return binder::Status::ok();
    }
private:
    inputdispatcher::InputDispatcher& mDispatcher;
};

class MotionReceiver {
public:
    MotionReceiver(std::unique_ptr<InputChannel> channel, const sp<SurfaceControl>& marker,
                   std::array<sp<SurfaceControl>, 4> cells)
          : mChannel(std::move(channel)), mConsumer(mChannel), mMarker(marker), mCells(std::move(cells)) {}

    void poll() {
        (void)mChannel->waitForMessage(10ms);
        for (int drained=0;drained<64;drained++) {
            InputEvent* raw = nullptr;
            uint32_t seq = 0;
            const auto [result, _unfinished] = mConsumer.consume(&mFactory, true, -1, &seq, &raw);
            if (!result.ok()) return;
            if (raw != nullptr && raw->getType() == InputEventType::MOTION) {
                const auto& event = static_cast<const MotionEvent&>(*raw);
                const int action = event.getActionMasked();
                const size_t pointers = event.getPointerCount();
                if (pointers >= 2) mSawTwoFingers = true;
                if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_MOVE ||
                    action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                    const float x = event.getX(0);
                    const float y = event.getY(0);
                    SurfaceComposerClient::Transaction().setPosition(mMarker, x - kMarkerSize / 2,
                                                                      y - kMarkerSize / 2).show(mMarker).apply();
                    std::printf("A6L_INPUT_MOTION action=%d pointers=%zu x=%.1f y=%.1f\n", action, pointers, x, y);
                    std::fflush(stdout);
                    mEvents++;
                }
                if (action == AMOTION_EVENT_ACTION_DOWN) {
                    const int quadrant = (event.getX(0) >= kWidth / 2 ? 1 : 0) +
                            (event.getY(0) >= kHeight / 2 ? 2 : 0);
                    mQuadrants |= 1u << quadrant;
                    SurfaceComposerClient::Transaction().setColor(mCells[quadrant], half3{0.f, 1.f, 0.f}).apply();
                    if (mQuadrants == 0xf) std::puts("A6L_INPUT_TARGETS_DONE quadrants=green");
                }
            }
            if (mConsumer.sendFinishedSignal(seq, true) != OK) fail("input ACK");
        }
    }
    int events() const { return mEvents; }
    bool sawTwoFingers() const { return mSawTwoFingers; }
private:
    std::shared_ptr<InputChannel> mChannel;
    InputConsumer mConsumer;
    PreallocatedInputEventFactory mFactory;
    sp<SurfaceControl> mMarker;
    std::array<sp<SurfaceControl>, 4> mCells;
    int mEvents = 0;
    bool mSawTwoFingers = false;
    unsigned mQuadrants = 0;
};

DisplayViewport makeViewport() {
    DisplayViewport viewport;
    viewport.displayId = ui::LogicalDisplayId::DEFAULT;
    viewport.orientation = ui::ROTATION_0;
    viewport.logicalRight = viewport.physicalRight = viewport.deviceWidth = kWidth;
    viewport.logicalBottom = viewport.physicalBottom = viewport.deviceHeight = kHeight;
    viewport.isActive = true;
    viewport.uniqueId = "local:0";
    viewport.type = ViewportType::INTERNAL;
    return viewport;
}
} // namespace

int main(int argc, char** argv) {
    std::setbuf(stdout, nullptr);
    alarm(180);
    const bool dumpOnly = argc==2 && !std::strcmp(argv[1], "--dump-only");
    if ((dumpOnly && getuid()!=0) || (!dumpOnly && (argc!=1 || getuid()!=1000))) fail("service identity");
    std::printf("A6L_INPUT_START uid=%u dump_only=%d\n", getuid(), dumpOnly);
    ProcessState::self()->startThreadPool();
    sp<IBinder> surfaceFlinger;
    for (int attempt = 0; attempt < 400 && !surfaceFlinger; ++attempt) {
        surfaceFlinger = defaultServiceManager()->checkService(String16("SurfaceFlinger"));
        if (!surfaceFlinger) usleep(100000);
    }
    if (!surfaceFlinger) fail("SurfaceFlinger service");
    if (dumpOnly) {
        int fd=open("/logs/surfaceflinger.dump",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(fd<0||surfaceFlinger->dump(fd,Vector<String16>{})!=OK)fail("SurfaceFlinger dump");
        close(fd);return 0;
    }
    std::puts("A6L_INPUT_STAGE surfaceflinger_ready");
    auto client = sp<SurfaceComposerClient>::make();
    if (client->initCheck() != OK) fail("SurfaceComposerClient");
    auto ids = SurfaceComposerClient::getPhysicalDisplayIds();
    if (ids.size() != 1) fail("one physical display");
    auto display = SurfaceComposerClient::getPhysicalDisplayToken(ids.front());
    if (!display) fail("physical display token");
    SurfaceComposerClient::setDisplayPowerMode(display, 2);

    a6l::DispatcherPolicy dispatcherPolicy;
    std::puts("A6L_INPUT_STAGE dispatcher_create");
    inputdispatcher::InputDispatcher dispatcher(dispatcherPolicy, nullptr);
    if (dispatcher.start() != OK) fail("InputDispatcher start");
    dispatcher.setInputDispatchMode(true, false);
    auto inputService=sp<InputService>::make(dispatcher);
    if(defaultServiceManager()->addService(String16("inputflinger"),inputService)!=OK)fail("input service registration");
    // In this private namespace SF only uses 'window' as a death-notification
    // token. This represents our native window owner, not a framework WM.
    auto windowOwner=sp<BBinder>::make();
    if(defaultServiceManager()->addService(String16("window"),windowOwner)!=OK)fail("window owner registration");
    auto channelResult = dispatcher.createInputChannel("a6l native input channel");
    if (!channelResult.ok()) fail("InputDispatcher channel");

    auto marker = client->createSurface(String8("A6L input marker"), kMarkerSize, kMarkerSize,
                                        PIXEL_FORMAT_RGBA_8888, ISurfaceComposerClient::eFXSurfaceEffect);
    auto inputLayer = client->createSurface(String8("A6L input target"), 0, 0,
                                            PIXEL_FORMAT_RGBA_8888, ISurfaceComposerClient::eFXSurfaceContainer);
    if (!marker || !inputLayer) fail("input layers");
    std::array<sp<SurfaceControl>, 4> cells;
    for (size_t index = 0; index < cells.size(); ++index) {
        cells[index] = client->createSurface(String8("A6L touch status"), 72, 72,
                                             PIXEL_FORMAT_RGBA_8888, ISurfaceComposerClient::eFXSurfaceEffect);
        if (!cells[index]) fail("status cell");
    }
    auto bootBuffer = client->createSurface(String8("A6L input boot buffer"), 16, 16,
                                            PIXEL_FORMAT_RGBA_8888, 0);
    if (!bootBuffer || !bootBuffer->isValid()) fail("boot buffer layer");

    auto info = sp<gui::WindowInfoHandle>::make();
    info->editInfo()->token = channelResult.value()->getConnectionToken();
    info->editInfo()->name = "A6L native InputDispatcher target";
    info->editInfo()->displayId = ui::LogicalDisplayId::DEFAULT;
    info->editInfo()->dispatchingTimeout = 5s;
    info->editInfo()->globalScaleFactor = 1.f;
    info->editInfo()->touchableRegion.orSelf(Rect(0, 0, kWidth, kHeight));
    info->editInfo()->applicationInfo.token = new BBinder();
    info->editInfo()->applicationInfo.name = "A6L native input";
    info->editInfo()->applicationInfo.dispatchingTimeoutMillis = 5000;

    // This pinned InputDispatcher registers its own SurfaceFlinger listener.
    // Do not register a second forwarder for the same window-info updates.

    SurfaceComposerClient::Transaction transaction;
    ui::LayerStack stack{0}; // Same logical display ID as the reader viewport.
    transaction.setDisplayLayerStack(display, stack);
    transaction.setDisplayProjection(display, ui::ROTATION_0, Rect(kWidth, kHeight), Rect(kWidth, kHeight));
    transaction.setLayerStack(bootBuffer, stack).setLayer(bootBuffer, 0).show(bootBuffer);
    transaction.setLayerStack(inputLayer, stack).setLayer(inputLayer, INT32_MAX - 20)
            .setCrop(inputLayer, Rect(kWidth,kHeight)).setInputWindowInfo(inputLayer, info).show(inputLayer);
    transaction.setLayerStack(marker, stack).setLayer(marker, INT32_MAX - 19).setColor(marker, half3{1.f, 1.f, 0.f})
            .setCrop(marker, Rect(kMarkerSize,kMarkerSize)).setPosition(marker, kWidth / 2 - kMarkerSize / 2, kHeight / 2 - kMarkerSize / 2).show(marker);
    for (size_t index = 0; index < cells.size(); ++index) {
        transaction.setLayerStack(cells[index], stack).setLayer(cells[index], INT32_MAX - 18)
                .setColor(cells[index], half3{1.f, 0.f, 0.f}).setCrop(cells[index], Rect(72,72))
                .setPosition(cells[index], index % 2 ? kWidth - 96 : 24,
                             index / 2 ? kHeight - 96 : 24).show(cells[index]);
    }
    if (transaction.apply(true) != OK) fail("input window transaction");
    auto surface = bootBuffer->getSurface();
    ANativeWindow_Buffer buffer{};
    if (surface->lock(&buffer, nullptr) != OK) fail("boot buffer lock");
    for (int y = 0; y < buffer.height; ++y) for (int x = 0; x < buffer.width; ++x)
        static_cast<uint32_t*>(buffer.bits)[y * buffer.stride + x] = 0xff000000;
    if (surface->unlockAndPost() != OK) fail("boot buffer post");
    if (SurfaceComposerClient::bootFinished()!=OK) fail("graphics boot finished");
    SurfaceComposerClient::Transaction().setInputWindowInfo(inputLayer,info).apply(true);
    std::puts("A6L_INPUT_STAGE graphics_boot_finished input_service_registered=1");

    InputReaderConfiguration readerConfiguration;
    readerConfiguration.setDisplayViewports({makeViewport()});
    auto readerPolicy = sp<a6l::ReaderPolicy>::make(readerConfiguration);
    InputReader reader(std::make_shared<EventHub>(), readerPolicy, dispatcher, nullptr, nullptr);
    if (reader.start() != OK) fail("InputReader start");
    MotionReceiver receiver(std::move(channelResult.value()), marker, cells);
    int ready = open("/logs/input-ready", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (ready < 0) fail("input ready marker");
    close(ready);
    std::puts("A6L_INPUT_READY pipeline=EventHub_InputReader_InputDispatcher_InputChannel hold=180s");

    for (int elapsed = 0; elapsed < 1800; ++elapsed) {
        receiver.poll();
        if (access("/logs/finish", F_OK) == 0) break;
        usleep(100000);
    }
    sleep(2);
    std::string inputDump;
    reader.dump(inputDump);
    dispatcher.dump(inputDump);
    FILE* inputFile = fopen("/logs/input.dump", "wx");
    if (!inputFile) fail("input dump open");
    if (fwrite(inputDump.data(), 1, inputDump.size(), inputFile) != inputDump.size()) fail("input dump write");
    if (fclose(inputFile)) fail("input dump close");
    int request=open("/logs/dump-request",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(request<0)fail("dump request");close(request);
    for(int retry=0;retry<150&&access("/logs/dump-done",F_OK);retry++)usleep(100000);
    if(access("/logs/dump-done",F_OK))fail("root dump helper timeout");
    std::puts("A6L_INPUT_FRAME_VISIBLE final_marker=1");
    sleep(3);
    std::printf("A6L_INPUT_PASS motion_events=%d two_finger=%d acked=1\n", receiver.events(), receiver.sawTwoFingers());
    reader.stop();
    dispatcher.stop();
    return receiver.events() > 0 ? 0 : 2;
}
