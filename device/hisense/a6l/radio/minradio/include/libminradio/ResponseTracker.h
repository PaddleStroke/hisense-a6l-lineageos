/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <aidl/android/hardware/radio/RadioError.h>
#include <aidl/android/hardware/radio/RadioResponseInfo.h>
#include <android-base/logging.h>
#include <android-base/thread_annotations.h>
#include <android/binder_auto_utils.h>
#include <libminradio/binder_printing.h>

#include <map>
#include <memory>

namespace android::hardware::radio::minimal {

class ResponseTrackerResultBase {
  private:
    const char* mDescriptor;
    ::aidl::android::hardware::radio::RadioError mError;

    static ::aidl::android::hardware::radio::RadioError toError(const ::ndk::ScopedAStatus& status);

  protected:
    ResponseTrackerResultBase(const char* descriptor);
    ResponseTrackerResultBase(const char* descriptor,
                              ::aidl::android::hardware::radio::RadioError error);
    ResponseTrackerResultBase(const char* descriptor, ::ndk::ScopedAStatus st);

  public:
    virtual ~ResponseTrackerResultBase() = default;

    bool isOk() const;
    bool expectOk() const;
    ::aidl::android::hardware::radio::RadioError getError() const;
    const char* getDescriptor() const;
};

template <typename ResultData>
class ResponseTrackerResult : public ResponseTrackerResultBase {
  private:
    ResultData mResultData;

  public:
    ResponseTrackerResult() : ResponseTrackerResultBase(ResultData::descriptor) {}
    ResponseTrackerResult(::ndk::ScopedAStatus st)
        : ResponseTrackerResultBase(ResultData::descriptor, std::move(st)) {}
    ResponseTrackerResult(ResultData data, ::aidl::android::hardware::radio::RadioError error)
        : ResponseTrackerResultBase(ResultData::descriptor, error), mResultData(data) {}

    const ResultData& get() const {
        CHECK(expectOk()) << "Request failed";
        return mResultData;
    }
    const ResultData& operator*() const { return get(); }
    const ResultData* operator->() const { return &get(); }
};

template <typename ResultData>
std::ostream& operator<<(std::ostream& os, const ResponseTrackerResult<ResultData>& val) {
    using namespace ::android::hardware::radio::minimal::binder_printing;
    if (val.isOk()) {
        return os << *val;
    } else {
        return os << "ResponseTrackerResult<" << val.getDescriptor()  //
                  << ">{error=" << val.getError() << "}";
    }
}

class ResponseTrackerBase {
  protected:
    class ScopedSerial;

  private:
    mutable std::mutex mSerialsGuard;
    int32_t mSerial GUARDED_BY(mSerialsGuard) = initialSerial();
    std::map<int32_t, std::unique_ptr<ResponseTrackerResultBase>> mTrackedSerials
            GUARDED_BY(mSerialsGuard);

    static int32_t initialSerial();
    ::ndk::ScopedAStatus handle(const ::aidl::android::hardware::radio::RadioResponseInfo& info,
                                std::unique_ptr<ResponseTrackerResultBase> result);
    std::unique_ptr<ResponseTrackerResultBase> getResultBase(ScopedSerial& serial);

  protected:
    class ScopedSerial {
      private:
        int32_t mSerial;
        bool mIsReleased = false;

        /* Raw pointer to allow ResponseTrackerBase self-reference. DISALLOW_COPY_AND_ASSIGN and
         * protected status of newSerial ensures ScopedSerial won't outlive mTracker. */
        ResponseTrackerBase* mTracker;

        DISALLOW_COPY_AND_ASSIGN(ScopedSerial);

      public:
        ScopedSerial(int32_t serial, ResponseTrackerBase* tracker);
        ~ScopedSerial();
        operator int32_t() const;
        void release();
    };

    ScopedSerial newSerial();
    bool isTracked(int32_t serial) const;
    void cancelTracking(ScopedSerial& serial);

    template <typename ResultData>
    ::ndk::ScopedAStatus handle(const ::aidl::android::hardware::radio::RadioResponseInfo& info,
                                const ResultData& data) {
        std::unique_ptr<ResponseTrackerResultBase> result =
                std::make_unique<ResponseTrackerResult<ResultData>>(data, info.error);
        return handle(info, std::move(result));
    }

    template <typename ResultData>
    ResponseTrackerResult<ResultData> getResult(ScopedSerial& serial) {
        auto baseResult = getResultBase(serial);
        if (!baseResult) return {};
        CHECK(baseResult->getDescriptor() == ResultData::descriptor)
                << "Failed to get ResponseTracker result. Expected " << ResultData::descriptor
                << ", but got " << baseResult->getDescriptor();
        return static_cast<ResponseTrackerResult<ResultData>&>(*baseResult);
    }
};

template <typename RequestInterface, typename ResponseInterface>
class ResponseTracker : public ResponseInterface::DefaultDelegator, protected ResponseTrackerBase {
  private:
    std::weak_ptr<RequestInterface> mRequest;

  protected:
    std::shared_ptr<RequestInterface> request() {
        auto req = mRequest.lock();
        CHECK(req) << "request() should only be called from RequestInterface context! "
                   << "Failing this check means RequestInterface has been free'd.";
        return req;
    }

  public:
    ResponseTracker(std::shared_ptr<RequestInterface> req,
                    const std::shared_ptr<ResponseInterface>& resp)
        : ResponseInterface::DefaultDelegator(resp), mRequest(req) {}
};

template <typename ResponseTrackerT>
class ResponseTrackerHolder {
  private:
    mutable std::mutex mResponseTrackerGuard;
    std::shared_ptr<ResponseTrackerT> mTracker GUARDED_BY(mResponseTrackerGuard);

  public:
    operator bool() const {
        std::unique_lock lck(mResponseTrackerGuard);
        return mTracker != nullptr;
    }

    ResponseTrackerHolder& operator=(std::shared_ptr<ResponseTrackerT> tracker) {
        std::unique_lock lck(mResponseTrackerGuard);
        mTracker = std::move(tracker);
        return *this;
    }

    std::shared_ptr<ResponseTrackerT> operator()() const {
        std::unique_lock lck(mResponseTrackerGuard);
        return mTracker;
    }

    std::shared_ptr<ResponseTrackerT> get() const {
        std::unique_lock lck(mResponseTrackerGuard);
        return mTracker;
    }
};

}  // namespace android::hardware::radio::minimal
