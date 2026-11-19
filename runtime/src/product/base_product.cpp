#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        "Sin & Punishment: Star Successor",
    };
    return descriptor;
}

} // namespace RuntimeProduct
