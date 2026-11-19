#pragma once

#include <string_view>

namespace RuntimeProduct {

struct Descriptor {
    std::string_view displayName;
};

// The single executable links one small provider definition (product/base_product.cpp).
const Descriptor& Active() noexcept;

} // namespace RuntimeProduct
