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

#include <ostream>

namespace android::hardware::radio::minimal::binder_printing {

namespace details {

template <typename _T>
class LooksLikeBinderStruct {
    template <typename _U>
    static auto _test(int) -> decltype(std::declval<_U>().writeToParcel(nullptr), std::true_type());
    template <typename _U>
    static std::false_type _test(...);

  public:
    enum { value = decltype(_test<_T>(0))::value };
};

template <typename _T>
class HasToStringMethod {
    template <typename _U>
    static auto _test(int) -> decltype(std::declval<_U>().toString(), std::true_type());
    template <typename _U>
    static std::false_type _test(...);

  public:
    enum { value = decltype(_test<_T>(0))::value };
};

template <typename _T>
class HasToStringFunction {
    template <typename _U>
    static auto _test(int) -> decltype(toString(std::declval<_U>()), std::true_type());
    template <typename _U>
    static std::false_type _test(...);

  public:
    enum { value = decltype(_test<_T>(0))::value };
};

}  // namespace details

template <typename T, typename = std::enable_if_t<details::LooksLikeBinderStruct<T>::value &&
                                                  details::HasToStringMethod<T>::value>>
std::ostream& operator<<(std::ostream& os, const T& val) {
    return os << val.toString();
}

template <typename T, typename = std::enable_if_t<details::LooksLikeBinderStruct<T>::value &&
                                                  details::HasToStringMethod<T>::value>>
std::ostream& operator<<(std::ostream& os, const std::optional<T>& val) {
    if (!val.has_value()) return os << "nullopt";
    return os << *val;
}

template <typename T,
          typename = std::enable_if_t<std::is_enum<T>::value &&
                                      details::HasToStringFunction<T>::value>,
          typename = void>
std::ostream& operator<<(std::ostream& os, T val) {
    return os << toString(val);
}

template <typename T, typename = std::enable_if_t<
                              (details::LooksLikeBinderStruct<T>::value &&
                               details::HasToStringMethod<T>::value) ||
                              (std::is_enum<T>::value && details::HasToStringFunction<T>::value) ||
                              std::is_same_v<T, int32_t> || std::is_same_v<T, std::string>>>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& val) {
    os << '[';
    bool first = true;
    for (auto&& el : val) {
        if (first) {
            first = false;
        } else {
            os << ", ";
        }
        os << el;
    }
    return os << ']';
}

}  // namespace android::hardware::radio::minimal::binder_printing
